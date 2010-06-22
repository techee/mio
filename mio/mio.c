/*
 *  MIO, an I/O abstraction layer replicating C file I/O API.
 *  Copyright (C) 2010  Colomban Wendling <ban@herbesfolles.org>
 * 
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include "mio.h"

#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>


/* minimal reallocation chunk size */
#define MIO_CHUNK_SIZE 4096



MIO *
mio_new_file (const gchar *path,
              const gchar *mode)
{
  FILE *fp;
  MIO  *mio = NULL;
  
  fp = fopen (path, mode);
  if (fp) {
    mio = g_slice_alloc (sizeof *mio);
    if (! mio) {
      fclose (fp);
    } else {
      mio->type = MIO_TYPE_FILE;
      mio->impl.file.fp = fp;
      mio->impl.file.close = TRUE;
    }
  }
  
  return mio;
}

MIO *
mio_new_fp (FILE     *fp,
            gboolean  do_close)
{
  MIO *mio;
  
  mio = g_slice_alloc (sizeof *mio);
  if (mio) {
    mio->type = MIO_TYPE_FILE;
    mio->impl.file.fp = fp;
    mio->impl.file.close = do_close;
  }
  
  return mio;
}

MIO *
mio_new_memory (guchar         *data,
                gsize           size,
                MIOReallocFunc  realloc_func,
                GDestroyNotify  free_func)
{
  MIO  *mio;
  
  mio = g_slice_alloc (sizeof *mio);
  if (mio) {
    mio->type = MIO_TYPE_MEMORY;
    mio->impl.mem.buf = data;
    mio->impl.mem.ungetch = EOF;
    mio->impl.mem.pos = 0;
    mio->impl.mem.size = size;
    mio->impl.mem.allocated_size = size;
    mio->impl.mem.realloc_func = realloc_func;
    mio->impl.mem.free_func = free_func;
    mio->impl.mem.eof = FALSE;
    mio->impl.mem.error = FALSE;
  }
  
  return mio;
}

void
mio_free (MIO *mio)
{
  if (mio) {
    switch (mio->type) {
      case MIO_TYPE_MEMORY:
        if (mio->impl.mem.free_func) {
          mio->impl.mem.free_func (mio->impl.mem.buf);
        }
        mio->impl.mem.buf = NULL;
        mio->impl.mem.pos = 0;
        mio->impl.mem.size = 0;
        mio->impl.mem.allocated_size = 0;
        mio->impl.mem.realloc_func = NULL;
        mio->impl.mem.free_func = NULL;
        mio->impl.mem.eof = FALSE;
        mio->impl.mem.error = FALSE;
        break;
      
      case MIO_TYPE_FILE:
        if (mio->impl.file.close) {
          fclose (mio->impl.file.fp);
        }
        mio->impl.file.close = FALSE;
        mio->impl.file.fp = NULL;
        break;
    }
    g_slice_free1 (sizeof *mio, mio);
  }
}

gsize
mio_read (MIO    *mio,
          void   *ptr,
          gsize   size,
          gsize   nmemb)
{
  gsize n_read = 0;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (size != 0 && nmemb != 0) {
        if (mio->impl.mem.ungetch != EOF) {
          *((guchar *)ptr) = (guchar)mio->impl.mem.ungetch;
          mio->impl.mem.ungetch = EOF;
          mio->impl.mem.pos++;
          if (size == 1) {
            n_read++;
          } else if (mio->impl.mem.pos + (size - 1) <= mio->impl.mem.size) {
            memcpy (&(((guchar *)ptr)[1]),
                    &mio->impl.mem.buf[mio->impl.mem.pos], size - 1);
            mio->impl.mem.pos += size - 1;
            n_read++;
          }
        }
        for (; n_read < nmemb; n_read++) {
          if (mio->impl.mem.pos + size > mio->impl.mem.size) {
            break;
          } else {
            memcpy (&(((guchar *)ptr)[n_read * size]),
                    &mio->impl.mem.buf[mio->impl.mem.pos], size);
            mio->impl.mem.pos += size;
          }
        }
        if (mio->impl.mem.pos >= mio->impl.mem.size) {
          mio->impl.mem.eof = TRUE;
        }
      }
      break;
    
    case MIO_TYPE_FILE:
      n_read = fread (ptr, size, nmemb, mio->impl.file.fp);
      break;
  }
  
  return n_read;
}

static gboolean
try_resize (MIO  *mio,
            gsize new_size)
{
  gboolean success = FALSE;
  
  if (mio->impl.mem.realloc_func) {
    if (G_UNLIKELY (new_size == G_MAXSIZE)) {
      #ifdef EOVERFLOW
      errno = EOVERFLOW;
      #endif
    } else {
      if (new_size > mio->impl.mem.size) {
        if (new_size <= mio->impl.mem.allocated_size) {
          mio->impl.mem.size = new_size;
          success = TRUE;
        } else {
          gsize   newsize;
          guchar *newbuf;
          
          newsize = MAX (mio->impl.mem.allocated_size + MIO_CHUNK_SIZE,
                         new_size);
          newbuf = mio->impl.mem.realloc_func (mio->impl.mem.buf, newsize);
          if (newbuf) {
            mio->impl.mem.buf = newbuf;
            mio->impl.mem.allocated_size = newsize;
            mio->impl.mem.size = new_size;
            success = TRUE;
          }
        }
      } else {
        guchar *newbuf;
        
        newbuf = mio->impl.mem.realloc_func (mio->impl.mem.buf, new_size);
        if (G_LIKELY (newbuf || new_size == 0)) {
          mio->impl.mem.buf = newbuf;
          mio->impl.mem.allocated_size = new_size;
          mio->impl.mem.size = new_size;
          success = TRUE;
        }
      }
    }
  }
  
  return success;
}

static gboolean
try_ensure_space (MIO  *mio,
                  gsize n)
{
  gboolean success = TRUE;
  
  if (mio->impl.mem.pos + n > mio->impl.mem.size) {
    success = try_resize (mio, mio->impl.mem.pos + n);
  }
  
  return success;
}

gsize
mio_write (MIO         *mio,
           const void  *ptr,
           gsize        size,
           gsize        nmemb)
{
  gsize n_written = 0;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (size != 0 && nmemb != 0) {
        if (try_ensure_space (mio, size * nmemb)) {
          memcpy (&mio->impl.mem.buf[mio->impl.mem.pos], ptr, size * nmemb);
          mio->impl.mem.pos += size * nmemb;
          n_written = nmemb;
        }
      }
      break;
    
    case MIO_TYPE_FILE:
      n_written = fwrite (ptr, size, nmemb, mio->impl.file.fp);
      break;
  }
  
  return n_written;
}

gint
mio_putc (MIO  *mio,
          gint  c)
{
  gint rv = EOF;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (try_ensure_space (mio, 1)) {
        mio->impl.mem.buf[mio->impl.mem.pos] = (guchar)c;
        mio->impl.mem.pos++;
        rv = (gint)((guchar)c);
      }
      break;
    
    case MIO_TYPE_FILE:
      rv = fputc (c, mio->impl.file.fp);
      break;
  }
  
  return rv;
}

gint
mio_puts (MIO          *mio,
          const gchar  *s)
{
  gint rv = EOF;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY: {
      gsize len;
      
      len = strlen (s);
      if (try_ensure_space (mio, len)) {
        memcpy (&mio->impl.mem.buf[mio->impl.mem.pos], s, len);
        mio->impl.mem.pos += len;
        rv = 1;
      }
      break;
    }
    
    case MIO_TYPE_FILE:
      rv = fputs (s, mio->impl.file.fp);
      break;
  }
  
  return rv;
}

gint
mio_vprintf (MIO         *mio,
             const gchar *format,
             va_list      ap)
{
  gint    rv = -1;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY: {
      gint    n;
      gchar   tmp;
      gsize   old_pos;
      gsize   old_size;
      va_list ap_copy;
      
      old_pos = mio->impl.mem.pos;
      old_size = mio->impl.mem.size;
      va_copy (ap_copy, ap);
      /* compute the size we will need into the buffer */
      n = vsnprintf (&tmp, 1, format, ap_copy);
      va_end (ap_copy);
      if (n >= 0 && try_ensure_space (mio, ((guint)n) + 1)) {
        guchar c;
        
        /* backup character at n+1 that will be overwritten by a \0 ... */
        c = mio->impl.mem.buf[mio->impl.mem.pos + (guint)n];
        rv = vsnprintf ((gchar *)&mio->impl.mem.buf[mio->impl.mem.pos],
                        (guint)n + 1, format, ap);
        /* ...and restore it */
        mio->impl.mem.buf[mio->impl.mem.pos + (guint)n] = c;
        if (G_LIKELY (rv >= 0 && rv == n)) {
          /* re-compute the actual size since we might have allocated one byte
           * more than needed */
          mio->impl.mem.size = MAX (old_size, old_pos + (guint)rv);
          mio->impl.mem.pos += (guint)rv;
        } else {
          mio->impl.mem.size = old_size;
          rv = -1;
        }
      }
      break;
    }
    
    case MIO_TYPE_FILE:
      rv = vfprintf (mio->impl.file.fp, format, ap);
      break;
  }
  
  return rv;
}

gint
mio_printf (MIO         *mio,
            const gchar *format,
            ...)
{
  gint    rv;
  va_list ap;
  
  va_start (ap, format);
  rv = mio_vprintf (mio, format, ap);
  va_end (ap);
  
  return rv;
}

gint
mio_getc (MIO *mio)
{
  gint rv = EOF;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (mio->impl.mem.ungetch != EOF) {
        rv = mio->impl.mem.ungetch;
        mio->impl.mem.ungetch = EOF;
        mio->impl.mem.pos++;
      } else if (mio->impl.mem.pos < mio->impl.mem.size) {
        rv = mio->impl.mem.buf[mio->impl.mem.pos];
        mio->impl.mem.pos++;
      } else {
        mio->impl.mem.eof = TRUE;
      }
      break;
    
    case MIO_TYPE_FILE:
      rv = fgetc (mio->impl.file.fp);
      break;
  }
  
  return rv;
}

gint
mio_ungetc (MIO  *mio,
            gint  ch)
{
  gint rv = EOF;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (ch != EOF && mio->impl.mem.ungetch == EOF) {
        rv = mio->impl.mem.ungetch = ch;
        mio->impl.mem.pos--;
        mio->impl.mem.eof = FALSE;
      }
      break;
    
    case MIO_TYPE_FILE:
      rv = ungetc (ch, mio->impl.file.fp);
      break;
  }
  
  return rv;
}

gchar *
mio_gets (MIO    *mio,
          gchar  *s,
          gsize   size)
{
  gchar *rv = NULL;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (size > 0) {
        gsize i = 0;
        
        if (mio->impl.mem.ungetch != EOF) {
          s[i] = (gchar)mio->impl.mem.ungetch;
          mio->impl.mem.ungetch = EOF;
          mio->impl.mem.pos++;
          i++;
        }
        for (; mio->impl.mem.pos < mio->impl.mem.size && i < (size - 1); i++) {
          s[i] = (gchar)mio->impl.mem.buf[mio->impl.mem.pos];
          mio->impl.mem.pos++;
          if (s[i] == '\n') {
            i++;
            break;
          }
        }
        if (i > 0) {
          s[i] = 0;
          rv = s;
        }
        if (mio->impl.mem.pos >= mio->impl.mem.size) {
          mio->impl.mem.eof = TRUE;
        }
      }
      break;
    
    case MIO_TYPE_FILE:
      if (size > G_MAXINT) {
        /* FIXME: report the error */
      } else {
        rv = fgets (s, (int)size, mio->impl.file.fp);
      }
      break;
  }
  
  return rv;
}

void
mio_clearerr (MIO *mio)
{
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      mio->impl.mem.error = FALSE;
      mio->impl.mem.eof = FALSE;
      break;
    
    case MIO_TYPE_FILE:
      clearerr (mio->impl.file.fp);
      break;
  }
}

gint
mio_eof (MIO *mio)
{
  gint rv = 1;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      rv = mio->impl.mem.eof != FALSE;
      break;
    
    case MIO_TYPE_FILE:
      rv = feof (mio->impl.file.fp);
      break;
  }
  
  return rv;
}

gint
mio_error (MIO *mio)
{
  gint rv = 1;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      rv = mio->impl.mem.error != FALSE;
      break;
    
    case MIO_TYPE_FILE:
      rv = ferror (mio->impl.file.fp);
      break;
  }
  
  return rv;
}

/* FIXME: should we support seeking out of bounds like lseek() seems to do? */
gint
mio_seek (MIO  *mio,
          glong offset,
          gint  whence)
{
  gint rv = -1;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      switch (whence) {
        case SEEK_SET:
          if (offset < 0 || (gsize)offset > mio->impl.mem.size) {
            errno = EINVAL;
          } else {
            mio->impl.mem.pos = offset;
            rv = 0;
          }
          break;
        
        case SEEK_CUR:
          if ((offset < 0 && (gsize)-offset > mio->impl.mem.pos) ||
              mio->impl.mem.pos + offset > mio->impl.mem.size) {
            errno = EINVAL;
          } else {
            mio->impl.mem.pos += offset;
            rv = 0;
          }
          break;
        
        case SEEK_END:
          if (offset > 0 || (gsize)-offset > mio->impl.mem.size) {
            errno = EINVAL;
          } else {
            mio->impl.mem.pos = mio->impl.mem.size - (gsize)-offset;
            rv = 0;
          }
          break;
        
        default:
          errno = EINVAL;
      }
      if (rv == 0) {
        mio->impl.mem.eof = FALSE;
        mio->impl.mem.ungetch = EOF;
      }
      break;
    
    case MIO_TYPE_FILE:
      rv = fseek (mio->impl.file.fp, offset, whence);
      break;
  }
  
  return rv;
}

glong
mio_tell (MIO *mio)
{
  glong rv = -1;
  
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      rv = mio->impl.mem.pos;
      break;
    
    case MIO_TYPE_FILE:
      rv = ftell (mio->impl.file.fp);
      break;
  }
  
  return rv;
}

void
mio_rewind (MIO *mio)
{
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      mio->impl.mem.pos = 0;
      mio->impl.mem.ungetch = EOF;
      mio->impl.mem.eof = FALSE;
      mio->impl.mem.error = FALSE;
      break;
    
    case MIO_TYPE_FILE:
      rewind (mio->impl.file.fp);
      break;
  }
}

gint
mio_getpos (MIO    *mio,
            MIOPos *pos)
{
  gint rv = -1;
  
  pos->type = mio->type;
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (mio->impl.mem.pos == (gsize)-1) {
        /* this happens if ungetc() was called at the start of the stream */
        #ifdef EIO
        errno = EIO;
        #endif
      } else {
        pos->impl.mem = mio->impl.mem.pos;
        rv = 0;
      }
      break;
    
    case MIO_TYPE_FILE:
      rv = fgetpos (mio->impl.file.fp, &pos->impl.file);
      break;
  }
  #ifdef MIO_DEBUG
  if (rv != -1) {
    pos->tag = mio;
  }
  #endif /* MIO_DEBUG */
  
  return rv;
}

gint
mio_setpos (MIO    *mio,
            MIOPos *pos)
{
  gint rv = -1;
  
  #ifdef MIO_DEBUG
  if (pos->tag != mio) {
    g_critical ("mio_setpos((MIO*)%p, (MIOPos*)%p): "
                "Given MIOPos was not set by a previous call to mio_getpos() "
                "on the same MIO object, which means there is a bug in "
                "someone's code.",
                (void *)mio, (void *)pos);
    errno = EINVAL;
    return -1;
  }
  #endif /* MIO_DEBUG */
  switch (mio->type) {
    case MIO_TYPE_MEMORY:
      if (pos->impl.mem > mio->impl.mem.size) {
        errno = EINVAL;
      } else {
        mio->impl.mem.ungetch = EOF;
        mio->impl.mem.pos = pos->impl.mem;
        rv = 0;
      }
      break;
    
    case MIO_TYPE_FILE:
      rv = fsetpos (mio->impl.file.fp, &pos->impl.file);
      break;
  }
  
  return rv;
}
