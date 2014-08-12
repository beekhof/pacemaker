/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef CIB_IO__H
#  define CIB_IO__H

#  include <stdio.h>
#  include <sys/types.h>
#  include <unistd.h>

#  include <stdlib.h>
#  include <errno.h>
#  include <fcntl.h>

#  include <crm/common/xml.h>
#  include <crm/common/mainloop.h>

extern gboolean initialized;
extern xmlNode *the_cib;
extern xmlNode *node_search;
extern xmlNode *resource_search;
extern xmlNode *constraint_search;
extern xmlNode *status_search;

extern xmlNode *get_the_CIB(void);

extern int initializeCib(xmlNode * cib);
extern gboolean uninitializeCib(void);
extern gboolean verifyCibXml(xmlNode * cib);
extern xmlNode *readCibXml(char *buffer);
extern xmlNode *readCibXmlFile(const char *dir, const char *file, gboolean discard_status);
extern int activateCibBuffer(char *buffer, const char *filename);
extern int activateCibXml(xmlNode * doc, gboolean to_disk, const char *op);
extern crm_trigger_t *cib_writer;
extern gboolean cib_writes_enabled;

/* extern xmlNode *server_get_cib_copy(void); */

/*
 * for OSs without support for direntry->d_type, like Solaris
 */
#ifndef DT_UNKNOWN
enum {
       DT_UNKNOWN = 0,
# define DT_UNKNOWN     DT_UNKNOWN
       DT_FIFO = 1,
# define DT_FIFO        DT_FIFO
       DT_CHR = 2,
# define DT_CHR         DT_CHR
       DT_DIR = 4,
# define DT_DIR         DT_DIR
       DT_BLK = 6,
# define DT_BLK         DT_BLK
       DT_REG = 8,
# define DT_REG         DT_REG
       DT_LNK = 10,
# define DT_LNK         DT_LNK
       DT_SOCK = 12,
# define DT_SOCK        DT_SOCK
       DT_WHT = 14
# define DT_WHT         DT_WHT
};
#endif /*DT_UNKNOWN*/


#endif
