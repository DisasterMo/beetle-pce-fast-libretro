/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "mednafen.h"
#include <string.h>
#include <sys/types.h>
#include "cdromif.h"
#include "CDAccess.h"
#include "general.h"

#include <algorithm>
#include "libretro.h"

extern retro_log_printf_t log_cb;

enum
{
   // Status/Error messages
   CDIF_MSG_DONE = 0,     // Read -> emu. args: No args.
   CDIF_MSG_INFO,         // Read -> emu. args: str_message
   CDIF_MSG_FATAL_ERROR,     // Read -> emu. args: *TODO ARGS*

   //
   // Command messages.
   //
   CDIF_MSG_DIEDIEDIE,    // Emu -> read

   CDIF_MSG_READ_SECTOR,     /* Emu -> read
               args[0] = lba
            */

   CDIF_MSG_EJECT,     // Emu -> read, args[0]; 0=insert, 1=eject
};

class CDIF_Message
{
public:

   CDIF_Message();
   CDIF_Message(unsigned int message_, uint32 arg0 = 0, uint32 arg1 = 0,
                uint32 arg2 = 0, uint32 arg3 = 0);
   CDIF_Message(unsigned int message_, const std::string &str);
   ~CDIF_Message();

   unsigned int message;
   uint32 args[4];
   void* parg;
   std::string str_message;
};

class CDIF_Queue
{
public:

   CDIF_Queue();
   ~CDIF_Queue();

   bool Read(CDIF_Message* message, bool blocking = TRUE);

   void Write(const CDIF_Message &message);

private:
   std::queue<CDIF_Message> ze_queue;
};


typedef struct
{
   bool valid;
   bool error;
   uint32 lba;
   uint8 data[2352 + 96];
} CDIF_Sector_Buffer;

// TODO: prohibit copy constructor
class CDIF_ST : public CDIF
{
public:

   CDIF_ST(CDAccess* cda);
   virtual ~CDIF_ST();

   virtual void HintReadSector(uint32 lba);
   virtual bool ReadRawSector(uint8* buf, uint32 lba);
   virtual bool Eject(bool eject_status);

private:
   CDAccess* disc_cdaccess;
};

CDIF::CDIF() : UnrecoverableError(false), is_phys_cache(false),
   DiscEjected(false)
{

}

CDIF::~CDIF()
{

}


CDIF_Message::CDIF_Message()
{
   message = 0;

   memset(args, 0, sizeof(args));
}

CDIF_Message::CDIF_Message(unsigned int message_, uint32 arg0, uint32 arg1,
                           uint32 arg2, uint32 arg3)
{
   message = message_;
   args[0] = arg0;
   args[1] = arg1;
   args[2] = arg2;
   args[3] = arg3;
}

CDIF_Message::CDIF_Message(unsigned int message_, const std::string &str)
{
   message = message_;
   str_message = str;
}

CDIF_Message::~CDIF_Message()
{

}

CDIF_Queue::CDIF_Queue()
{
}

CDIF_Queue::~CDIF_Queue()
{
}

// Returns FALSE if message not read, TRUE if it was read.  Will always return TRUE if "blocking" is set.
// Will throw MDFN_Error if the read message code is CDIF_MSG_FATAL_ERROR
bool CDIF_Queue::Read(CDIF_Message* message, bool blocking)
{
   bool ret = true;

   if (ze_queue.size() == 0)
      ret = false;
   else
   {
      *message = ze_queue.front();
      ze_queue.pop();
   }

   assert(!ret || message->message != CDIF_MSG_FATAL_ERROR);
   //   if(ret && message->message == CDIF_MSG_FATAL_ERROR)
   //      throw MDFN_Error(0, "%s", message->str_message.c_str());

   return (ret);
}

void CDIF_Queue::Write(const CDIF_Message &message)
{
   ze_queue.push(message);

}



bool CDIF::ValidateRawSector(uint8* buf)
{
   int mode = buf[12 + 3];

   if (mode != 0x1 && mode != 0x2)
      return (false);

   if (!edc_lec_check_and_correct(buf, mode == 2))
      return (false);

   return (true);
}


int CDIF::ReadSector(uint8* pBuf, uint32 lba, uint32 nSectors)
{
   int ret = 0;

   if (UnrecoverableError)
      return (false);

   while (nSectors--)
   {
      uint8 tmpbuf[2352 + 96];

      if (!ReadRawSector(tmpbuf, lba))
      {
         puts("CDIF Raw Read error");
         return (FALSE);
      }

      if (!ValidateRawSector(tmpbuf))
      {
         if (log_cb)
         {
            log_cb(RETRO_LOG_ERROR, "Uncorrectable data at sector %d\n", lba);
            log_cb(RETRO_LOG_ERROR, "Uncorrectable data at sector %d\n", lba);
         }
         return (false);
      }

      const int mode = tmpbuf[12 + 3];

      if (!ret)
         ret = mode;

      if (mode == 1)
         memcpy(pBuf, &tmpbuf[12 + 4], 2048);
      else if (mode == 2)
         memcpy(pBuf, &tmpbuf[12 + 4 + 8], 2048);
      else
      {
         printf("CDIF_ReadSector() invalid sector type at LBA=%u\n", (unsigned int)lba);
         return (false);
      }

      pBuf += 2048;
      lba++;
   }

   return (ret);
}

//
//
// Single-threaded implementation follows.
//
//

CDIF_ST::CDIF_ST(CDAccess* cda) : disc_cdaccess(cda)
{
   //puts("***WARNING USING SINGLE-THREADED CD READER***");

   is_phys_cache = false;
   UnrecoverableError = false;
   DiscEjected = false;

   disc_cdaccess->Read_TOC(&disc_toc);

   assert(disc_toc.first_track > 0 && disc_toc.last_track < 100
          && disc_toc.first_track <= disc_toc.last_track);
   //  throw(MDFN_Error(0, ("TOC first(%d)/last(%d) track numbers bad."), disc_toc.first_track, disc_toc.last_track));
}

CDIF_ST::~CDIF_ST()
{
   if (disc_cdaccess)
   {
      delete disc_cdaccess;
      disc_cdaccess = NULL;
   }
}

void CDIF_ST::HintReadSector(uint32 lba)
{
   // TODO: disc_cdaccess seek hint? (probably not, would require asynchronousitycamel)
}

bool CDIF_ST::ReadRawSector(uint8* buf, uint32 lba)
{
   if (UnrecoverableError)
   {
      memset(buf, 0, 2352 + 96);
      return (false);
   }

   try
   {
      disc_cdaccess->Read_Raw_Sector(buf, lba);
   }
   catch (std::exception &e)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Sector %u read error: %s\n", lba, e.what());
      memset(buf, 0, 2352 + 96);
      return (false);
   }

   return (true);
}

bool CDIF_ST::Eject(bool eject_status)
{
   if (UnrecoverableError)
      return (false);

   try
   {
      int32 old_de = DiscEjected;

      DiscEjected = eject_status;

      if (old_de != DiscEjected)
      {
         disc_cdaccess->Eject(eject_status);

         if (!eject_status)    // Re-read the TOC
         {
            disc_cdaccess->Read_TOC(&disc_toc);

            assert(disc_toc.first_track > 0 && disc_toc.last_track < 100
                   && disc_toc.first_track <= disc_toc.last_track);
            //     throw(MDFN_Error(0, ("TOC first(%d)/last(%d) track numbers bad."), disc_toc.first_track, disc_toc.last_track));
         }
      }
   }
   catch (std::exception &e)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "%s\n", e.what());
      return (false);
   }

   return (true);
}


class CDIF_Stream_Thing : public Stream
{
public:

   CDIF_Stream_Thing(CDIF* cdintf_arg, uint32 lba_arg, uint32 sector_count_arg);
   ~CDIF_Stream_Thing();

   virtual uint64 attributes(void);
   virtual uint8* map(void);
   virtual void unmap(void);

   virtual uint64 read(void* data, uint64 count, bool error_on_eos = true);
   virtual void write(const void* data, uint64 count);

   virtual void seek(int64 offset, int whence);
   virtual int64 tell(void);
   virtual int64 size(void);
   virtual void close(void);

private:
   CDIF* cdintf;
   const uint32 start_lba;
   const uint32 sector_count;
   int64 position;
};

CDIF_Stream_Thing::CDIF_Stream_Thing(CDIF* cdintf_arg, uint32 start_lba_arg,
                                     uint32 sector_count_arg) : cdintf(cdintf_arg), start_lba(start_lba_arg),
   sector_count(sector_count_arg)
{

}

CDIF_Stream_Thing::~CDIF_Stream_Thing()
{

}

uint64 CDIF_Stream_Thing::attributes(void)
{
   return (ATTRIBUTE_READABLE | ATTRIBUTE_SEEKABLE);
}

uint8* CDIF_Stream_Thing::map(void)
{
   return NULL;
}

void CDIF_Stream_Thing::unmap(void)
{

}

uint64 CDIF_Stream_Thing::read(void* data, uint64 count, bool error_on_eos)
{
   if (count > (((uint64)sector_count * 2048) - position))
   {
      assert(!error_on_eos);
      //   throw MDFN_Error(0, "EOF");

      count = ((uint64)sector_count * 2048) - position;
   }

   if (!count)
      return (0);

   for (uint64 rp = position; rp < (position + count); rp = (rp & ~ 2047) + 2048)
   {
      uint8 buf[2048];

      if (!cdintf->ReadSector(buf, start_lba + (rp / 2048), 1))
      {
         assert(false);
         //   throw MDFN_Error(ErrnoHolder(EIO));
      }

      //::printf("Meow: %08llx -- %08llx\n", count, (rp - position) + std::min<uint64>(2048 - (rp & 2047), count - (rp - position)));
      memcpy((uint8*)data + (rp - position), buf + (rp & 2047),
             std::min<uint64>(2048 - (rp & 2047), count - (rp - position)));
   }

   position += count;

   return count;
}

void CDIF_Stream_Thing::write(const void* data, uint64 count)
{
   assert(false);
   // throw MDFN_Error(ErrnoHolder(EBADF));
}

void CDIF_Stream_Thing::seek(int64 offset, int whence)
{
   int64 new_position;

   switch (whence)
   {
   default:
      assert(false);
      // throw MDFN_Error(ErrnoHolder(EINVAL));
      break;

   case SEEK_SET:
      new_position = offset;
      break;

   case SEEK_CUR:
      new_position = position + offset;
      break;

   case SEEK_END:
      new_position = ((int64)sector_count * 2048) + offset;
      break;
   }

   assert(new_position >= 0 && new_position <= ((int64)sector_count * 2048));
   //  throw MDFN_Error(ErrnoHolder(EINVAL));

   position = new_position;
}

int64 CDIF_Stream_Thing::tell(void)
{
   return position;
}

int64 CDIF_Stream_Thing::size(void)
{
   return (sector_count * 2048);
}

void CDIF_Stream_Thing::close(void)
{

}


Stream* CDIF::MakeStream(uint32 lba, uint32 sector_count)
{
   return new CDIF_Stream_Thing(this, lba, sector_count);
}


CDIF* CDIF_Open(const char* path)
{
   CDAccess* cda = cdaccess_open_image(path);

   //single threaded reader :
   return new CDIF_ST(cda);
}
