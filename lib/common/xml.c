/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>
#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <libxml/xmlreader.h>

#if HAVE_BZLIB_H
#  include <bzlib.h>
#endif

#if HAVE_LIBXML2
#  include <libxml/parser.h>
#  include <libxml/tree.h>
#  include <libxml/relaxng.h>
#endif

#if HAVE_LIBXSLT
#  include <libxslt/xslt.h>
#  include <libxslt/transform.h>
#endif

#define XML_BUFFER_SIZE	4096
#define XML_PARSER_DEBUG 0
#define BEST_EFFORT_STATUS 0

enum xml_log_options 
{
    xml_log_option_formatted  = 0x01,
    xml_log_option_diff_plus  = 0x02,
    xml_log_option_diff_minus = 0x04,
    xml_log_option_diff_short = 0x10,
    xml_log_option_diff_all   = 0x20,
};

void xml_log(int priority, const char * fmt, ...) G_GNUC_PRINTF(2,3);

void xml_log(int priority, const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    qb_log_from_external_source_va(__FUNCTION__, __FILE__, fmt, priority, __LINE__, 0, ap);
    va_end(ap);
}

typedef struct 
{
	xmlRelaxNGPtr rng;
	xmlRelaxNGValidCtxtPtr valid;
	xmlRelaxNGParserCtxtPtr parser;
} relaxng_ctx_cache_t;

struct schema_s 
{
	int type;
	const char *name;
	const char *location;
	const char *transform;
	int after_transform;
	void *cache;
};

struct schema_s known_schemas[] = {
    /* 0 */    { 0, NULL, NULL, NULL, 1 },
    /* 1 */    { 1, "pacemaker-0.6",    "crm.dtd",		"upgrade06.xsl", 4, NULL },
    /* 2 */    { 1, "transitional-0.6", "crm-transitional.dtd",	"upgrade06.xsl", 4, NULL },
    /* 3 */    { 2, "pacemaker-0.7",    "pacemaker-1.0.rng",	NULL, 0, NULL },
    /* 4 */    { 2, "pacemaker-1.0",    "pacemaker-1.0.rng",	NULL, 6, NULL },
    /* 5 */    { 2, "pacemaker-1.1",    "pacemaker-1.1.rng",	NULL, 6, NULL },
    /* 6 */    { 2, "pacemaker-1.2",    "pacemaker-1.2.rng",	NULL, 0, NULL },
    /* 7 */    { 0, "none", NULL, NULL, 0, NULL },
};

static int all_schemas = DIMOF(known_schemas);
static int max_schemas = DIMOF(known_schemas) - 2; /* skip back past 'none' */


					      typedef struct  
					      {
						      int found;
						      const char *string;
					      } filter_t;
					      
static filter_t filter[] = {
    { 0, XML_ATTR_ORIGIN },
    { 0, XML_CIB_ATTR_WRITTEN },		
    { 0, XML_ATTR_UPDATE_ORIG },
    { 0, XML_ATTR_UPDATE_CLIENT },
    { 0, XML_ATTR_UPDATE_USER },
};

static char *get_schema_path(const char *file) 
{
    static const char *base = NULL;
    if(base == NULL) {
        base = getenv("PCMK_schema_directory");
    }
    if(base == NULL || strlen(base) == 0) {
        base = CRM_DTD_DIRECTORY;
    }
    return crm_concat(base, file, '/');
}

int print_spaces(char *buffer, int spaces, int max);

int get_tag_name(const char *input, size_t offset, size_t max);
int get_attr_name(const char *input, size_t offset, size_t max);
int get_attr_value(const char *input, size_t offset, size_t max);
gboolean can_prune_leaf(xmlNode *xml_node);

void diff_filter_context(int context, int upper_bound, int lower_bound,
			 xmlNode *xml_node, xmlNode *parent);
int in_upper_context(int depth, int context, xmlNode *xml_node);
int write_file(const char *string, const char *filename);
xmlNode *subtract_xml_object(xmlNode *parent, xmlNode *left, xmlNode *right, gboolean full, const char *marker);
int add_xml_object(xmlNode *parent, xmlNode *target, xmlNode *update, gboolean as_diff);

static inline const char *
crm_attr_value(xmlAttr *attr)
{
    if(attr == NULL || attr->children == NULL) {
	return NULL;
    }
    return (const char*)attr->children->content;
}

static inline xmlAttr *
crm_first_attr(xmlNode *xml)
{
    if(xml == NULL) {
	return NULL;
    }
    return xml->properties;
}

xmlNode *
find_xml_node(xmlNode *root, const char * search_path, gboolean must_find)
{
    xmlNode *a_child = NULL;
    const char *name = "NULL";
    if(root != NULL) {
	name = crm_element_name(root);
    }
	
    if(search_path == NULL) {
	crm_warn("Will never find <NULL>");
	return NULL;
    }
	
    for(a_child = __xml_first_child(root); a_child != NULL; a_child = __xml_next(a_child)) {
	if(crm_str_eq((const char *)a_child->name, search_path, TRUE)) {
/* 		crm_trace("returning node (%s).", crm_element_name(a_child)); */
	    return a_child;
	}
    }

    if(must_find) {
	crm_warn("Could not find %s in %s.", search_path, name);
    } else if(root != NULL) {
	crm_trace("Could not find %s in %s.", search_path, name);
    } else {
	crm_trace("Could not find %s in <NULL>.", search_path);
    }
	
	
    return NULL;
}

xmlNode*
find_entity(xmlNode *parent, const char *node_name, const char *id)
{
    xmlNode *a_child = NULL;
    for(a_child = __xml_first_child(parent); a_child != NULL; a_child = __xml_next(a_child)) {
	/* Uncertain if node_name == NULL check is strictly necessary here */
	if(node_name == NULL || crm_str_eq((const char *)a_child->name, node_name, TRUE)) {
	    if(id == NULL || crm_str_eq(id, ID(a_child), TRUE)) {
		crm_trace("returning node (%s).", 
			    crm_element_name(a_child));
		return a_child;
	    }
	}
    }
    
    crm_trace("node <%s id=%s> not found in %s.",
		node_name, id, crm_element_name(parent));
    return NULL;
}

void
copy_in_properties(xmlNode* target, xmlNode *src)
{
    if(src == NULL) {
	crm_warn("No node to copy properties from");

    } else if (target == NULL) {
	crm_err("No node to copy properties into");

    } else {
        xmlAttrPtr pIter = NULL;
        for(pIter = crm_first_attr(src); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = crm_attr_value(pIter);
            
            expand_plus_plus(target, p_name, p_value);
        }
    }
	
    return;
}

void fix_plus_plus_recursive(xmlNode* target)
{
    /* TODO: Remove recursion and use xpath searches for value++ */
    xmlNode *child = NULL;
    xmlAttrPtr pIter = NULL;
    for(pIter = crm_first_attr(target); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;
        const char *p_value = crm_attr_value(pIter);
            
        expand_plus_plus(target, p_name, p_value);
    }
    for(child = __xml_first_child(target); child != NULL; child = __xml_next(child)) {
	fix_plus_plus_recursive(child);
    }
}


void
expand_plus_plus(xmlNode* target, const char *name, const char *value)
{
    int offset = 1;
    int name_len = 0;
    int int_value = 0;
    int value_len = 0;

    const char *old_value = NULL;

    if(value == NULL || name == NULL) {
	return;
    }
    
    old_value = crm_element_value(target, name);

    if(old_value == NULL) {
	/* if no previous value, set unexpanded */
	goto set_unexpanded;

    } else if(strstr(value, name) != value) {
	goto set_unexpanded;
    }

    name_len = strlen(name);
    value_len = strlen(value);
    if(value_len < (name_len + 2)
       || value[name_len] != '+'
       || (value[name_len+1] != '+' && value[name_len+1] != '=')) {
	goto set_unexpanded;
    }

    /* if we are expanding ourselves,
     * then no previous value was set and leave int_value as 0
     */
    if(old_value != value) {
	int_value = char2score(old_value);
    }
    
    if(value[name_len+1] != '+') {
	const char *offset_s = value+(name_len+2);
	offset = char2score(offset_s);
    }
    int_value += offset;

    if(int_value > INFINITY) {
	int_value = INFINITY;
    }
    
    crm_xml_add_int(target, name, int_value);
    return;

  set_unexpanded:
    if(old_value == value) {
	/* the old value is already set, nothing to do */
	return;
    }
    crm_xml_add(target, name, value);
    return;
}

xmlDoc *getDocPtr(xmlNode *node)
{
    xmlDoc *doc = NULL;
    CRM_CHECK(node != NULL, return NULL);

    doc = node->doc;
    if(doc == NULL) {
	doc = xmlNewDoc((const xmlChar*)"1.0");
	xmlDocSetRootElement(doc, node);
	xmlSetTreeDoc(node, doc);
    }
    return doc;
}

xmlNode*
add_node_copy(xmlNode *parent, xmlNode *src_node) 
{
    xmlNode *child = NULL;
    xmlDoc *doc = getDocPtr(parent);
    CRM_CHECK(src_node != NULL, return NULL);

    child = xmlDocCopyNode(src_node, doc, 1);
    xmlAddChild(parent, child);
    return child;
}


int
add_node_nocopy(xmlNode *parent, const char *name, xmlNode *child)
{
    add_node_copy(parent, child);
    free_xml(child);
    return 1;
}

const char *
crm_xml_add(xmlNode* node, const char *name, const char *value)
{
    xmlAttr *attr = NULL;
    CRM_CHECK(node != NULL, return NULL);
    CRM_CHECK(name != NULL, return NULL);

    if(value == NULL) {
	return NULL;
    }

#if XML_PARANOIA_CHECKS
    {
	const char *old_value = NULL;
	old_value = crm_element_value(node, name);
	
	/* Could be re-setting the same value */
	CRM_CHECK(old_value != value,
		  crm_err("Cannot reset %s with crm_xml_add(%s)",
			  name, value);
		  return value);
    }
#endif
    
    attr = xmlSetProp(node, (const xmlChar*)name, (const xmlChar*)value);
    CRM_CHECK(attr && attr->children && attr->children->content, return NULL);
    return (char *)attr->children->content;
}

const char *
crm_xml_replace(xmlNode* node, const char *name, const char *value)
{
    xmlAttr *attr = NULL;
    const char *old_value = NULL;
    CRM_CHECK(node != NULL, return NULL);
    CRM_CHECK(name != NULL && name[0] != 0, return NULL);

    old_value = crm_element_value(node, name);

    /* Could be re-setting the same value */
    CRM_CHECK(old_value != value, return value);

    if (old_value != NULL && value == NULL) {
	xml_remove_prop(node, name);
	return NULL;

    } else if(value == NULL) {
	return NULL;
    }
    
    attr = xmlSetProp(node, (const xmlChar*)name, (const xmlChar*)value);
    CRM_CHECK(attr && attr->children && attr->children->content, return NULL);
    return (char *)attr->children->content;
}

const char *
crm_xml_add_int(xmlNode* node, const char *name, int value)
{
    char *number = crm_itoa(value);
    const char *added = crm_xml_add(node, name, number);
    free(number);
    return added;
}

xmlNode*
create_xml_node(xmlNode *parent, const char *name)
{
    xmlDoc *doc = NULL;
    xmlNode *node = NULL;	

    if (name == NULL || name[0] == 0) {
	return NULL;
    }
    
    if(parent == NULL) {
	doc = xmlNewDoc((const xmlChar*)"1.0");
	node = xmlNewDocRawNode(doc, NULL, (const xmlChar*)name, NULL);
	xmlDocSetRootElement(doc, node);
	
    } else {
	doc = getDocPtr(parent);
	node = xmlNewDocRawNode(doc, NULL, (const xmlChar*)name, NULL);
	xmlAddChild(parent, node);
    }
    return node;
}


void
free_xml(xmlNode * child) 
{
    if(child != NULL) {
        xmlNode *top = NULL;
        xmlDoc *doc = child->doc;

        if (doc != NULL) {
            top = xmlDocGetRootElement(doc);
        }

        if(doc != NULL && top == child) {
            /* Free everything */
            xmlFreeDoc(doc);

        } else {
            /* Free this particular subtree
             * Make sure to unlink it from the parent first
             */
            xmlUnlinkNode(child);
            xmlFreeNode(child);
        }
    }
}

xmlNode*
copy_xml(xmlNode *src)
{
    xmlDoc *doc = xmlNewDoc((const xmlChar*)"1.0");
    xmlNode *copy = xmlDocCopyNode(src, doc, 1);
    xmlDocSetRootElement(doc, copy);
    xmlSetTreeDoc(copy, doc);
    return copy;
}


static void crm_xml_err(void * ctx, const char * msg, ...) G_GNUC_PRINTF(2,3);

int
write_file(const char *string, const char *filename) 
{
    int rc = 0;
    FILE *file_output_strm = NULL;
	
    CRM_CHECK(filename != NULL, return -1);

    if (string == NULL) {
	crm_err("Cannot write NULL to %s", filename);
	return -1;
    }

    file_output_strm = fopen(filename, "w");
    if(file_output_strm == NULL) {
	crm_perror(LOG_ERR,"Cannot open %s for writing", filename);
	return -1;
    } 

    rc = fprintf(file_output_strm, "%s", string);
    if(rc < 0) {
	crm_perror(LOG_ERR,"Cannot write output to %s", filename);
    }		
	
    if(fflush(file_output_strm) != 0) {
	crm_perror(LOG_ERR,"fflush for %s failed:", filename);
	rc = -1;
    }
	
    if(fsync(fileno(file_output_strm)) < 0) {
	crm_perror(LOG_ERR,"fsync for %s failed:", filename);
	rc = -1;
    }
	    
    fclose(file_output_strm);
    return rc;
}

static void crm_xml_err(void * ctx, const char * msg, ...)
{
    int len = 0;
    va_list args;
    char *buf = NULL;
    static int buffer_len = 0;
    static char *buffer = NULL;
    
    va_start(args, msg);
    len = vasprintf(&buf, msg, args);

    if(strchr(buf, '\n')) {
	buf[len - 1] = 0;
	if(buffer) {
	    crm_err("XML Error: %s%s", buffer, buf);
	    free(buffer);
	} else {
	    crm_err("XML Error: %s", buf);	    
	}
	buffer = NULL;
	buffer_len = 0;
	
    } else if(buffer == NULL) {
	buffer_len = len;
	buffer = buf;
	buf = NULL;

    } else {
	buffer = realloc(buffer, 1+buffer_len+len);
	memcpy(buffer+buffer_len, buf, len);
	buffer_len += len;
        buffer[buffer_len] = 0;
    }
    
    va_end(args);
    free(buf);	
}

xmlNode*
string2xml(const char *input)
{
    xmlNode *xml = NULL;
    xmlDocPtr output = NULL;
    xmlParserCtxtPtr ctxt = NULL;
    xmlErrorPtr last_error = NULL;
	
    if(input == NULL) {
	crm_err("Can't parse NULL input");
	return NULL;
    }
	
    /* create a parser context */
    ctxt = xmlNewParserCtxt();
    CRM_CHECK(ctxt != NULL, return NULL);

    /* xmlCtxtUseOptions(ctxt, XML_PARSE_NOBLANKS|XML_PARSE_RECOVER); */

    xmlCtxtResetLastError(ctxt);
    xmlSetGenericErrorFunc(ctxt, crm_xml_err);
    /* initGenericErrorDefaultFunc(crm_xml_err); */
    output = xmlCtxtReadDoc(ctxt, (const xmlChar*)input, NULL, NULL, XML_PARSE_NOBLANKS|XML_PARSE_RECOVER);
    if(output) {
	xml = xmlDocGetRootElement(output);
    }
    last_error = xmlCtxtGetLastError(ctxt);
    if(last_error && last_error->code != XML_ERR_OK) {
	/* crm_abort(__FILE__,__PRETTY_FUNCTION__,__LINE__, "last_error->code != XML_ERR_OK", TRUE, TRUE); */
	/*
	 * http://xmlsoft.org/html/libxml-xmlerror.html#xmlErrorLevel
	 * http://xmlsoft.org/html/libxml-xmlerror.html#xmlParserErrors
	 */
	crm_warn("Parsing failed (domain=%d, level=%d, code=%d): %s",
		 last_error->domain, last_error->level,
		 last_error->code, last_error->message);

        if(last_error->code == XML_ERR_DOCUMENT_EMPTY) {
	    crm_abort(__FILE__,__PRETTY_FUNCTION__,__LINE__, "Cannot parse an empty string", TRUE, TRUE);

        } else if(last_error->code != XML_ERR_DOCUMENT_END) {
	    crm_err("Couldn't%s parse %d chars: %s", xml?" fully":"", (int)strlen(input), input);
	    if(xml != NULL) {
		crm_log_xml_err(xml, "Partial");
	    }

	} else {
	    int len = strlen(input);
	    crm_warn("String start: %.50s", input);
	    crm_warn("String start+%d: %s", len-50, input+len-50);
	    crm_abort(__FILE__,__PRETTY_FUNCTION__,__LINE__, "String parsing error", TRUE, TRUE);
	}
    }

    xmlFreeParserCtxt(ctxt);
    return xml;
}

xmlNode *
stdin2xml(void) 
{
    size_t data_length = 0;
    size_t read_chars = 0;
  
    char *xml_buffer = NULL;
    xmlNode *xml_obj = NULL;
  
    do {
	xml_buffer = realloc(xml_buffer, XML_BUFFER_SIZE + data_length + 1);
	read_chars = fread(xml_buffer + data_length, 1, XML_BUFFER_SIZE, stdin);
	data_length += read_chars;
    } while (read_chars > 0);

    if(data_length == 0) {
	crm_warn("No XML supplied on stdin");
	free(xml_buffer);
	return NULL;
    }

    xml_buffer[data_length] = '\0';

    xml_obj = string2xml(xml_buffer);
    free(xml_buffer);

    crm_log_xml_trace(xml_obj, "Created fragment");
    return xml_obj;
}

static char *
decompress_file(const char *filename)
{
    char *buffer = NULL;
#if HAVE_BZLIB_H
    int rc = 0;
    size_t length = 0, read_len = 0;
    
    BZFILE *bz_file = NULL;
    FILE *input = fopen(filename, "r");

    if(input == NULL) {
	crm_perror(LOG_ERR,"Could not open %s for reading", filename);
	return NULL;
    }
    
    bz_file = BZ2_bzReadOpen(&rc, input, 0, 0, NULL, 0);

    if ( rc != BZ_OK ) {
	BZ2_bzReadClose ( &rc, bz_file);
	return NULL;
    }
    
    rc = BZ_OK;
    while ( rc == BZ_OK ) {
	buffer = realloc(buffer, XML_BUFFER_SIZE + length + 1);
	read_len = BZ2_bzRead (
	    &rc, bz_file, buffer + length, XML_BUFFER_SIZE);
	
	crm_trace("Read %ld bytes from file: %d",
		    (long)read_len, rc);
	
	if ( rc == BZ_OK || rc == BZ_STREAM_END) {
	    length += read_len;
	}
    }
    
    buffer[length] = '\0';
    read_len = length;
    
    if ( rc != BZ_STREAM_END ) {
	crm_err("Couldnt read compressed xml from file");
	free(buffer);
	buffer = NULL;
    }
    
    BZ2_bzReadClose (&rc, bz_file);
    fclose(input);
    
#else
    crm_err("Cannot read compressed files:"
	    " bzlib was not available at compile time");
#endif
    return buffer;
}

static void strip_text_nodes(xmlNode *xml) 
{
    xmlNode *iter = xml->children;

    while (iter) {
        xmlNode *next = iter->next;

        switch(iter->type) {
            case XML_TEXT_NODE:
                /* Remove it */
                xmlUnlinkNode(iter);
                xmlFreeNode(iter);
                break;

            case XML_ELEMENT_NODE:
                /* Search it */
                strip_text_nodes(iter);
                break;

            default:
                /* Leave it */
                break;
        }

        iter = next;
    }
}

xmlNode *
filename2xml(const char *filename)
{
    xmlNode *xml = NULL;
    xmlDocPtr output = NULL;
    const char *match = NULL;
    xmlParserCtxtPtr ctxt = NULL;
    xmlErrorPtr last_error = NULL;
    static int xml_options = XML_PARSE_NOBLANKS|XML_PARSE_RECOVER;

    /* create a parser context */
    ctxt = xmlNewParserCtxt();
    CRM_CHECK(ctxt != NULL, return NULL);
    
    /* xmlCtxtUseOptions(ctxt, XML_PARSE_NOBLANKS|XML_PARSE_RECOVER); */
    
    xmlCtxtResetLastError(ctxt);
    xmlSetGenericErrorFunc(ctxt, crm_xml_err);
    /* initGenericErrorDefaultFunc(crm_xml_err); */

    if(filename) {
        match = strstr(filename, ".bz2");
    }

    if(filename == NULL) {
	/* STDIN_FILENO == fileno(stdin) */
	output = xmlCtxtReadFd(ctxt, STDIN_FILENO, "unknown.xml", NULL, xml_options);

    } else if(match == NULL || match[4] != 0) {
	output = xmlCtxtReadFile(ctxt, filename, NULL, xml_options);

    } else {
	char *input = decompress_file(filename);
	output = xmlCtxtReadDoc(ctxt, (const xmlChar*)input, NULL, NULL, xml_options);
	free(input);
    }

    if(output && (xml = xmlDocGetRootElement(output))) {
        strip_text_nodes(xml);
    }
    
    last_error = xmlCtxtGetLastError(ctxt);
    if(last_error && last_error->code != XML_ERR_OK) {
	/* crm_abort(__FILE__,__PRETTY_FUNCTION__,__LINE__, "last_error->code != XML_ERR_OK", TRUE, TRUE); */
	/*
	 * http://xmlsoft.org/html/libxml-xmlerror.html#xmlErrorLevel
	 * http://xmlsoft.org/html/libxml-xmlerror.html#xmlParserErrors
	 */
	crm_err("Parsing failed (domain=%d, level=%d, code=%d): %s",
		last_error->domain, last_error->level,
		last_error->code, last_error->message);
	
	if(last_error && last_error->code != XML_ERR_OK) {
	    crm_err("Couldn't%s parse %s", xml?" fully":"", filename);
	    if(xml != NULL) {
		crm_log_xml_err(xml, "Partial");
	    }
	}
    }
    
    xmlFreeParserCtxt(ctxt);
    return xml;
}

int
write_xml_file(xmlNode *xml_node, const char *filename, gboolean compress) 
{
    int res = 0;
    time_t now;
    char *buffer = NULL;
    char *now_str = NULL;
    unsigned int out = 0;
    FILE *file_output_strm = NULL;
    static mode_t cib_mode = S_IRUSR|S_IWUSR;
	
    CRM_CHECK(filename != NULL, return -1);

    crm_trace("Writing XML out to %s", filename);
    if (xml_node == NULL) {
	crm_err("Cannot write NULL to %s", filename);
	return -1;
    }

    file_output_strm = fopen(filename, "w");
    if(file_output_strm == NULL) {
	crm_perror(LOG_ERR,"Cannot open %s for writing", filename);
	return -1;
    } 

    /* establish the correct permissions */
    fchmod(fileno(file_output_strm), cib_mode);
	
    crm_log_xml_trace(xml_node, "Writing out");
	
    now = time(NULL);
    now_str = ctime(&now);
    now_str[24] = EOS; /* replace the newline */
    crm_xml_add(xml_node, XML_CIB_ATTR_WRITTEN, now_str);
	
    buffer = dump_xml_formatted(xml_node);
    CRM_CHECK(buffer != NULL && strlen(buffer) > 0,
	      crm_log_xml_warn(xml_node, "dump:failed");
	      goto bail);	

    if(compress) {
#if HAVE_BZLIB_H
	int rc = BZ_OK;
	unsigned int in = 0;
	BZFILE *bz_file = NULL;
	bz_file = BZ2_bzWriteOpen(&rc, file_output_strm, 5, 0, 30);
	if(rc != BZ_OK) {
	    crm_err("bzWriteOpen failed: %d", rc);
	} else {
	    BZ2_bzWrite(&rc,bz_file,buffer,strlen(buffer));
	    if(rc != BZ_OK) {
		crm_err("bzWrite() failed: %d", rc);
	    }
	}
	    
	if(rc == BZ_OK) {
	    BZ2_bzWriteClose(&rc, bz_file, 0, &in, &out);
	    if(rc != BZ_OK) {
		crm_err("bzWriteClose() failed: %d",rc);
		out = -1;
	    } else {
		crm_trace("%s: In: %d, out: %d", filename, in, out);
	    }
	}
#else
	crm_err("Cannot write compressed files:"
		" bzlib was not available at compile time");		
#endif
    }
	
    if(out <= 0) {
	res = fprintf(file_output_strm, "%s", buffer);
	if(res < 0) {
	    crm_perror(LOG_ERR,"Cannot write output to %s", filename);
	    goto bail;
	}		
    }
	
  bail:
	
    if(fflush(file_output_strm) != 0) {
	crm_perror(LOG_ERR,"fflush for %s failed:", filename);
	res = -1;
    }
	
    if(fsync(fileno(file_output_strm)) < 0) {
	crm_perror(LOG_ERR,"fsync for %s failed:", filename);
	res = -1;
    }
	    
    fclose(file_output_strm);
	
    crm_trace("Saved %d bytes to the Cib as XML", res);
    free(buffer);

    return res;
}

xmlNode *
get_message_xml(xmlNode *msg, const char *field) 
{
    xmlNode *tmp = first_named_child(msg, field);
    return __xml_first_child(tmp);
}

gboolean
add_message_xml(xmlNode *msg, const char *field, xmlNode *xml) 
{
    xmlNode *holder = create_xml_node(msg, field);
    add_node_copy(holder, xml);
    return TRUE;
}

static char *
dump_xml(xmlNode *an_xml_node, gboolean formatted, gboolean for_digest)
{
    int len = 0;
    char *buffer = NULL;
    xmlBuffer *xml_buffer = NULL;
    xmlDoc *doc = getDocPtr(an_xml_node);

    /* doc will only be NULL if an_xml_node is */
    CRM_CHECK(doc != NULL, return NULL);

    xml_buffer = xmlBufferCreate();
    CRM_ASSERT(xml_buffer != NULL);
    
    len = xmlNodeDump(xml_buffer, doc, an_xml_node, 0, formatted);

    if(len > 0) {
	/* The copying here isn't ideal, but it doesn't even register
	 * in the perf numbers
	 */
	if(for_digest) {
	    /* for compatability with the old result which is used for digests */
	    len += 3;
	    buffer = calloc(1, len);
	    snprintf(buffer, len, " %s\n", (char *)xml_buffer->content);
	} else {
	    buffer = strdup((char *)xml_buffer->content);	    
	}

    } else {
	crm_err("Conversion failed");
    }

    xmlBufferFree(xml_buffer);
    return buffer;    
}

char *
dump_xml_formatted(xmlNode *an_xml_node)
{
    return dump_xml(an_xml_node, TRUE, FALSE);
}

char *
dump_xml_unformatted(xmlNode *an_xml_node)
{
    return dump_xml(an_xml_node, FALSE, FALSE);
}
    
#define update_buffer() do {						\
	if(printed < 0) {						\
	    crm_perror(LOG_ERR,"snprintf failed");			\
	    goto print;							\
	} else if(printed >= (buffer_len - offset)) {			\
	    crm_err("Output truncated: available=%d, needed=%d", buffer_len - offset, printed);	\
	    offset += printed;						\
	    goto print;							\
	} else if(offset >= buffer_len) {				\
	    crm_err("Buffer exceeded");					\
	    offset += printed;						\
	    goto print;							\
	} else {							\
	    offset += printed;						\
	}								\
    } while(0)

int
print_spaces(char *buffer, int depth, int max) 
{
    int lpc = 0;
    int spaces = 2*depth;
    max--;
	
    /* <= so that we always print 1 space - prevents problems with syslog */
    for(lpc = 0; lpc <= spaces && lpc < max; lpc++) {
	if(sprintf(buffer+lpc, "%c", ' ') < 1) {
	    return -1;
	}
    }
    return lpc;
}

int
log_data_element(
    int log_level, const char *file, const char *function, int line,
    const char *prefix, xmlNode *data, int depth, int options)
{
    xmlNode *a_child = NULL;

    int offset = 0;
    int printed = 0;
    char *buffer = NULL;
    char *prefix_m = NULL;
    int buffer_len = 1000;

    xmlAttrPtr pIter = NULL;
    const char *name = NULL;
    const char *hidden = NULL;

    /* Since we use the same file and line, to avoid confusing libqb, we need to use the same format strings */
    if(data == NULL) {
        do_crm_log_alias(log_level, file, function, line, "%s%s", prefix, ": No data to dump as XML");
	return 0;
    }

    if(prefix == NULL) {
        prefix = "";
    }

    name = crm_element_name(data);
    CRM_ASSERT(name != NULL);
	
    /* crm_trace("Dumping %s", name); */
    buffer = calloc(1, buffer_len);
	
    if(is_set(options, xml_log_option_formatted)) {
	offset = print_spaces(buffer, depth, buffer_len - offset);
        if(is_set(options, xml_log_option_diff_plus) && (data->children == NULL || crm_element_value(data, XML_DIFF_MARKER))) {
            options |= xml_log_option_diff_all;
            prefix_m = strdup(prefix);
            prefix_m[1] = '+';
            prefix = prefix_m;

        } else if(is_set(options, xml_log_option_diff_minus) && (data->children == NULL || crm_element_value(data, XML_DIFF_MARKER))) {
            options |= xml_log_option_diff_all;
            prefix_m = strdup(prefix);
            prefix_m[1] = '-';
            prefix = prefix_m;
        }
    }

    if(is_set(options, xml_log_option_diff_short) && is_not_set(options, xml_log_option_diff_all)) {
        /* Still searching for the actual change */
        for(a_child = __xml_first_child(data); a_child != NULL; a_child = __xml_next(a_child)) {
            log_data_element(
                log_level, file, function, line, prefix, a_child, depth+1, options);
        }
        goto done;
    }

    printed = snprintf(buffer + offset, buffer_len - offset, "<%s", name);
    update_buffer();
	
    hidden = crm_element_value(data, "hidden");
    for(pIter = crm_first_attr(data); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;
        const char *p_value = crm_attr_value(pIter);
            
	if(p_name == NULL || safe_str_eq(F_XML_TAGNAME, p_name)) {
	    continue;

        } else if((is_set(options, xml_log_option_diff_plus) || is_set(options, xml_log_option_diff_minus))
                  && safe_str_eq(XML_DIFF_MARKER, p_name)) {
            continue;

	} else if(hidden != NULL
		  && p_name[0] != 0
		  && strstr(hidden, p_name) != NULL) {
	    p_value = "*****";
	}
		
	/* crm_trace("Dumping <%s %s=\"%s\"...", */
	/* 	    name, prop_name, prop_value); */
	printed = snprintf(buffer + offset, buffer_len - offset,
			   " %s=\"%s\"", p_name, p_value);
	update_buffer();
    }

    printed = snprintf(buffer + offset, buffer_len - offset,
		       " %s>", xml_has_children(data)?"":"/");
    update_buffer();
	
  print:
    do_crm_log_alias(log_level, file, function, line, "%s%s", prefix, buffer);
	
    if(xml_has_children(data) == FALSE) {
        free(prefix_m);
	free(buffer);
	return 0;
    }

    for(a_child = __xml_first_child(data); a_child != NULL; a_child = __xml_next(a_child)) {
	log_data_element(
	    log_level, file, function, line, prefix, a_child, depth+1, options);
    }

    if(is_set(options, xml_log_option_formatted)) {
	offset = print_spaces(buffer, depth, buffer_len);
    }

    printed = snprintf(buffer + offset, buffer_len - offset, "</%s>", name);
    update_buffer();

    do_crm_log_alias(log_level, file, function, line, "%s%s", prefix, buffer);

  done:
    free(prefix_m);
    free(buffer);
    return 1;
}

gboolean
xml_has_children(const xmlNode *xml_root)
{
    if(xml_root != NULL && xml_root->children != NULL) {
	return TRUE;
    }
    return FALSE;
}

int
crm_element_value_int(xmlNode *data, const char *name, int *dest)
{
    const char *value = crm_element_value(data, name);
    CRM_CHECK(dest != NULL, return -1);
    if(value) {
	*dest = crm_int_helper(value, NULL);
	return 0;
    }
    return -1;
}

int
crm_element_value_const_int(const xmlNode *data, const char *name, int *dest)
{
    return crm_element_value_int((xmlNode*)data, name, dest);
}

const char *
crm_element_value_const(const xmlNode *data, const char *name)
{
    return crm_element_value((xmlNode*)data, name);
}

char *
crm_element_value_copy(xmlNode *data, const char *name)
{
    char *value_copy = NULL;
    const char *value = crm_element_value(data, name);
    if(value != NULL) {
	value_copy = strdup(value);
    }
    return value_copy;
}

void
xml_remove_prop(xmlNode *obj, const char *name)
{
    xmlUnsetProp(obj, (const xmlChar*)name);
}

void
log_xml_diff(unsigned int log_level, xmlNode *diff, const char *function)
{
    xmlNode *child = NULL;
    xmlNode *added = find_xml_node(diff, "diff-added", FALSE);
    xmlNode *removed = find_xml_node(diff, "diff-removed", FALSE);
    gboolean is_first = TRUE;
    int options = xml_log_option_formatted;

    static struct qb_log_callsite *diff_cs = NULL;

    if(diff_cs == NULL) {
        diff_cs = qb_log_callsite_get(function, __FILE__, "xml-diff", log_level, __LINE__, 0);
    }

    if (crm_is_callsite_active(diff_cs, log_level, 0) == FALSE) {
        return;
    }

    if(log_level < LOG_DEBUG || function == NULL) {
        options |= xml_log_option_diff_short;
    }
    for(child = __xml_first_child(removed); child != NULL; child = __xml_next(child)) {
	log_data_element(log_level, __FILE__, function, __LINE__, "- ", child, 0, options|xml_log_option_diff_minus);
	if(is_first) {
	    is_first = FALSE;
	} else {
	    do_crm_log(log_level, " --- ");
	}
    }
    
    is_first = TRUE;
    for(child = __xml_first_child(added); child != NULL; child = __xml_next(child)) {
	log_data_element(log_level, __FILE__, function, __LINE__, "+ ", child, 0, options|xml_log_option_diff_plus);
	if(is_first) {
	    is_first = FALSE;
	} else {
	    do_crm_log(log_level, " +++ ");
	}
    }
}

void
purge_diff_markers(xmlNode *a_node)
{
    xmlNode *child = NULL;
    CRM_CHECK(a_node != NULL, return);

    xml_remove_prop(a_node, XML_DIFF_MARKER);
    for(child = __xml_first_child(a_node); child != NULL; child = __xml_next(child)) {
	purge_diff_markers(child);
    }
}

static void
save_xml_to_file(xmlNode *xml, const char *desc, const char *filename) 
{
    char *f = NULL;
    FILE *st = NULL;
    xmlDoc *doc = getDocPtr(xml);
    xmlBuffer *xml_buffer = xmlBufferCreate();

    if(filename == NULL) {
        char *uuid = crm_generate_uuid();
        f = g_strdup_printf("/tmp/%s", uuid);
        filename = f;
        free(uuid);
    }

    crm_info("Saving %s to %s", desc, filename);
    xmlNodeDump(xml_buffer, doc, xml, 0, FALSE);

    st = fopen(filename, "w");
    if(st) {
        fprintf(st, "%s", xml_buffer->content);
        /* fflush(st); */
        /* fsync(fileno(st)); */
        fclose(st);
    }

    xmlBufferFree(xml_buffer);
    g_free(f);
}

gboolean
apply_xml_diff(xmlNode *old, xmlNode *diff, xmlNode **new)
{
    gboolean result = TRUE;
    int root_nodes_seen = 0;
    static struct qb_log_callsite *digest_cs = NULL;
    const char *digest = crm_element_value(diff, XML_ATTR_DIGEST);
    const char *version = crm_element_value(diff, XML_ATTR_CRM_VERSION);

    xmlNode *child_diff = NULL;
    xmlNode *added = find_xml_node(diff, "diff-added", FALSE);
    xmlNode *removed = find_xml_node(diff, "diff-removed", FALSE);

    CRM_CHECK(new != NULL, return FALSE);
    if(digest_cs == NULL) {
        digest_cs = qb_log_callsite_get(__func__, __FILE__, "diff-digest", LOG_TRACE, __LINE__, 0);
    }

    crm_trace("Substraction Phase");
    for(child_diff = __xml_first_child(removed); child_diff != NULL; child_diff = __xml_next(child_diff)) {
	CRM_CHECK(root_nodes_seen == 0, result = FALSE);
	if(root_nodes_seen == 0) {
	    *new = subtract_xml_object(NULL, old, child_diff, FALSE, NULL);
	}
	root_nodes_seen++;
    }
    
    if(root_nodes_seen == 0) {
	*new = copy_xml(old);
		
    } else if(root_nodes_seen > 1) {
	crm_err("(-) Diffs cannot contain more than one change set..."
		" saw %d", root_nodes_seen);
	result = FALSE;
    }

    root_nodes_seen = 0;
    crm_trace("Addition Phase");
    if(result) {
	xmlNode *child_diff = NULL;
	for(child_diff = __xml_first_child(added); child_diff != NULL; child_diff = __xml_next(child_diff)) {
	    CRM_CHECK(root_nodes_seen == 0, result = FALSE);
	    if(root_nodes_seen == 0) {
		add_xml_object(NULL, *new, child_diff, TRUE);
	    }
	    root_nodes_seen++;
	}
    }

    if(root_nodes_seen > 1) {
	crm_err("(+) Diffs cannot contain more than one change set..."
		" saw %d", root_nodes_seen);
	result = FALSE;

    } else if(result && digest) {
	char *new_digest = NULL;
	purge_diff_markers(*new); /* Purge now so the diff is ok */
	new_digest = calculate_xml_versioned_digest(*new, FALSE, TRUE, version);
	if(safe_str_neq(new_digest, digest)) {
	    crm_info("Digest mis-match: expected %s, calculated %s",
		     digest, new_digest);
	    result = FALSE;

            crm_trace("%p %0.6x", digest_cs, digest_cs?digest_cs->targets:0);
            if (digest_cs && digest_cs->targets) {
                save_xml_to_file(old, "diff:original", NULL);
                save_xml_to_file(diff,"diff:input", NULL);
                save_xml_to_file(*new,"diff:new", NULL);
            }

        } else {
	    crm_trace("Digest matched: expected %s, calculated %s",
			digest, new_digest);
	}
	free(new_digest);
    }

    return result;
}


xmlNode *
diff_xml_object(xmlNode *old, xmlNode *new, gboolean suppress)
{
    xmlNode *tmp1    = NULL;
    xmlNode *diff    = create_xml_node(NULL, "diff");
    xmlNode *removed = create_xml_node(diff, "diff-removed");
    xmlNode *added   = create_xml_node(diff, "diff-added");

    crm_xml_add(diff, XML_ATTR_CRM_VERSION, CRM_FEATURE_SET);
	
    tmp1 = subtract_xml_object(removed, old, new, FALSE, "removed:top");
    if(suppress && tmp1 != NULL && can_prune_leaf(tmp1)) {
	free_xml(tmp1);
    }
	
    tmp1 = subtract_xml_object(added, new, old, TRUE, "added:top");
    if(suppress && tmp1 != NULL && can_prune_leaf(tmp1)) {
	free_xml(tmp1);
    }
	
    if(added->children == NULL && removed->children == NULL) {
	free_xml(diff);
	diff = NULL;
    }
	
    return diff;
}

gboolean
can_prune_leaf(xmlNode *xml_node)
{
    xmlNode *child = NULL;
    xmlAttrPtr pIter = NULL;
    gboolean can_prune = TRUE;

    for(pIter = crm_first_attr(xml_node); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;
        if(safe_str_eq(p_name, XML_ATTR_ID)) {
            continue;
        }		      
        can_prune = FALSE;
    }

    for(child = __xml_first_child(xml_node); child != NULL; child = __xml_next(child)) {
	if(can_prune_leaf(child)) {
	    free_xml(child);
	} else {
	    can_prune = FALSE;
	}
    }
    return can_prune;
}


void
diff_filter_context(int context, int upper_bound, int lower_bound,
		    xmlNode *xml_node, xmlNode *parent) 
{
    xmlNode *us = NULL;
    xmlNode *child = NULL;
    xmlAttrPtr pIter = NULL;
    xmlNode *new_parent = parent;
    const char *name = crm_element_name(xml_node);

    CRM_CHECK(xml_node != NULL && name != NULL, return);
	
    us = create_xml_node(parent, name);
    for(pIter = crm_first_attr(xml_node); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;
        const char *p_value = crm_attr_value(pIter);
            
        lower_bound = context;
        crm_xml_add(us, p_name, p_value);
    }

    if(lower_bound >= 0 || upper_bound >= 0) {
	crm_xml_add(us, XML_ATTR_ID, ID(xml_node));
	new_parent = us;

    } else {
	upper_bound = in_upper_context(0, context, xml_node);
	if(upper_bound >= 0) {
	    crm_xml_add(us, XML_ATTR_ID, ID(xml_node));
	    new_parent = us;
	} else {
	    free_xml(us);
	    us = NULL;
	}
    }

    for(child = __xml_first_child(us); child != NULL; child = __xml_next(child)) {
	diff_filter_context(context, upper_bound-1, lower_bound-1,
			    child, new_parent);
    }
}

int
in_upper_context(int depth, int context, xmlNode *xml_node)
{
    if(context == 0) {
	return 0;
    }

    if(xml_node->properties) {
	return depth;

    } else if(depth < context) {
	xmlNode *child = NULL;
	for(child = __xml_first_child(xml_node); child != NULL; child = __xml_next(child)) {
	    if(in_upper_context(depth+1, context, child)) {
		return depth;
	    }
	}
    }
    return 0;       
}


xmlNode *
subtract_xml_object(xmlNode *parent, xmlNode *left, xmlNode *right, gboolean full, const char *marker)
{
    gboolean skip = FALSE;
    gboolean differences = FALSE;
    xmlNode *diff = NULL;
    xmlNode *child_diff = NULL;
    xmlNode *right_child = NULL;
    xmlNode *left_child = NULL;
    xmlAttrPtr xIter = NULL;

    const char *id = NULL;
    const char *name = NULL;
    const char *value = NULL;
    const char *right_val = NULL;

    int lpc = 0;
    static int filter_len = DIMOF(filter);
	
    if(left == NULL) {
	return NULL;
    }

    id = ID(left);
    if(right == NULL) {
	xmlNode *deleted = NULL;

	crm_trace("Processing <%s id=%s> (complete copy)",
		    crm_element_name(left), id);
	deleted = add_node_copy(parent, left);
	crm_xml_add(deleted, XML_DIFF_MARKER, marker);

	return deleted;
    }

    name = crm_element_name(left);
    CRM_CHECK(name != NULL, return NULL);

    /* check for XML_DIFF_MARKER in a child */ 
    value = crm_element_value(right, XML_DIFF_MARKER);
    if(value != NULL && safe_str_eq(value, "removed:top")) {
        crm_trace("We are the root of the deletion: %s.id=%s", name, id);
	free_xml(diff);
	return NULL;
    }

    /* Avoiding creating the full heirarchy would save even more work here */
    diff = create_xml_node(parent, name);
	
    /* Reset filter */
    for(lpc = 0; lpc < filter_len; lpc++){
	filter[lpc].found = FALSE;
    }

    /* changes to child objects */
    for(left_child = __xml_first_child(left); left_child != NULL; left_child = __xml_next(left_child)) {
	right_child = find_entity(
	    right, crm_element_name(left_child), ID(left_child));
	child_diff = subtract_xml_object(diff, left_child, right_child, full, marker);
	if(child_diff != NULL) {
	    differences = TRUE;
	}
    }

    if(differences == FALSE) {
        /* Nothing to do */

    } else if(full) {
        xmlAttrPtr pIter = NULL;
        for(pIter = crm_first_attr(left); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = crm_attr_value(pIter);
            xmlSetProp(diff, (const xmlChar*)p_name, (const xmlChar*)p_value);
        }

	/* We already have everything we need... */
	goto done;

    } else if(id) {
	xmlSetProp(diff, (const xmlChar*)XML_ATTR_ID, (const xmlChar*)id);
    }
	
    /* changes to name/value pairs */
    for(xIter = crm_first_attr(left); xIter != NULL; xIter = xIter->next) {
        const char *prop_name = (const char *)xIter->name;

	if(crm_str_eq(prop_name, XML_ATTR_ID, TRUE)) {
	    continue;
	}

	skip = FALSE;
	for(lpc = 0; skip == FALSE && lpc < filter_len; lpc++){
	    if(filter[lpc].found == FALSE && crm_str_eq(prop_name, filter[lpc].string, TRUE)) {
		filter[lpc].found = TRUE;
		skip = TRUE;
		break;
	    }
	}
		      
	if(skip) { continue; }
		      
	right_val = crm_element_value(right, prop_name);
	if(right_val == NULL) {
	    /* new */
	    differences = TRUE;
	    if(full) {
                xmlAttrPtr pIter = NULL;
                for(pIter = crm_first_attr(left); pIter != NULL; pIter = pIter->next) {
                    const char *p_name = (const char *)pIter->name;
                    const char *p_value = crm_attr_value(pIter);
                    xmlSetProp(diff, (const xmlChar*)p_name, (const xmlChar*)p_value);
                }
		break;
			      
	    } else {
		const char *left_value = crm_element_value(left, prop_name);
		xmlSetProp(diff, (const xmlChar*)prop_name, (const xmlChar*)value);
		crm_xml_add(diff, prop_name, left_value);
	    }
				      
	} else {
	    /* Only now do we need the left value */
	    const char *left_value = crm_element_value(left, prop_name);
	    if(strcmp(left_value, right_val) == 0) {
		/* unchanged */

	    } else {
		/* changed */
		differences = TRUE;
		if(full) {
                    xmlAttrPtr pIter = NULL;
                    for(pIter = crm_first_attr(left); pIter != NULL; pIter = pIter->next) {
                        const char *p_name = (const char *)pIter->name;
                        const char *p_value = crm_attr_value(pIter);

                        xmlSetProp(diff, (const xmlChar*)p_name, (const xmlChar*)p_value);
                    }
                    break;
				  
		} else {
		    crm_xml_add(diff, prop_name, left_value);
		}
	    }
	}
    }

    if(differences == FALSE) {
	free_xml(diff);
	crm_trace("\tNo changes to <%s id=%s>", crm_str(name), id);
	return NULL;

    } else if(full == FALSE && id) {
	crm_xml_add(diff, XML_ATTR_ID, id);
    }
  done:	
    return diff;
}

int
add_xml_object(xmlNode *parent, xmlNode *target, xmlNode *update, gboolean as_diff)
{
    xmlNode *a_child = NULL;
    const char *object_id = NULL;
    const char *object_name = NULL;

#if XML_PARSE_DEBUG
    crm_log_xml_trace("update:", update);
    crm_log_xml_trace("target:", target);
#endif

    CRM_CHECK(update != NULL, return 0);

    object_name = crm_element_name(update);
    object_id = ID(update);

    CRM_CHECK(object_name != NULL, return 0);
	
    if(target == NULL && object_id == NULL) {
	/*  placeholder object */
	target = find_xml_node(parent, object_name, FALSE);

    } else if(target == NULL) {
	target = find_entity(parent, object_name, object_id);
    }

    if(target == NULL) {
	target = create_xml_node(parent, object_name);
	CRM_CHECK(target != NULL, return 0);
#if XML_PARSER_DEBUG
	crm_trace("Added  <%s%s%s/>", crm_str(object_name),
		    object_id?" id=":"", object_id?object_id:"");

    } else {
	crm_trace("Found node <%s%s%s/> to update",
		    crm_str(object_name),
		    object_id?" id=":"", object_id?object_id:"");
#endif
    }

    if(as_diff == FALSE) {
	/* So that expand_plus_plus() gets called */
	copy_in_properties(target, update);

    } else {
	/* No need for expand_plus_plus(), just raw speed */
        xmlAttrPtr pIter = NULL;
        for(pIter = crm_first_attr(update); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = crm_attr_value(pIter);
            /* Remove it first so the ordering of the update is preserved */
            xmlUnsetProp(target, (const xmlChar*)p_name);
            xmlSetProp(target, (const xmlChar*)p_name, (const xmlChar*)p_value);
        }
    }
	
    for(a_child = __xml_first_child(update); a_child != NULL; a_child = __xml_next(a_child)) {
#if XML_PARSER_DEBUG
	crm_trace("Updating child <%s id=%s>",
		    crm_element_name(a_child), ID(a_child));
#endif
	add_xml_object(target, NULL, a_child, as_diff);
    }

#if XML_PARSER_DEBUG
    crm_trace("Finished with <%s id=%s>",
		crm_str(object_name), crm_str(object_id));
#endif
    return 0;
}

gboolean
update_xml_child(xmlNode *child, xmlNode *to_update)
{
    gboolean can_update = TRUE;
    xmlNode *child_of_child = NULL;
	
    CRM_CHECK(child != NULL, return FALSE);
    CRM_CHECK(to_update != NULL, return FALSE);
	
    if(safe_str_neq(crm_element_name(to_update), crm_element_name(child))) {
	can_update = FALSE;

    } else if(safe_str_neq(ID(to_update), ID(child))) {
	can_update = FALSE;

    } else if(can_update) {
#if XML_PARSER_DEBUG
	crm_log_xml_trace(child, "Update match found...");
#endif
	add_xml_object(NULL, child, to_update, FALSE);
    }
	
    for(child_of_child = __xml_first_child(child); child_of_child != NULL; child_of_child = __xml_next(child_of_child)) {
	/* only update the first one */
	if(can_update) {
	    break;
	}
	can_update = update_xml_child(child_of_child, to_update);
    }
	
    return can_update;
}


int
find_xml_children(xmlNode **children, xmlNode *root,
		  const char *tag, const char *field, const char *value,
		  gboolean search_matches)
{
    int match_found = 0;
	
    CRM_CHECK(root != NULL, return FALSE);
    CRM_CHECK(children != NULL, return FALSE);
	
    if(tag != NULL && safe_str_neq(tag, crm_element_name(root))) {

    } else if(value != NULL
	      && safe_str_neq(value, crm_element_value(root, field))) {

    } else {
	if(*children == NULL) {
	    *children = create_xml_node(NULL, __FUNCTION__);
	}
	add_node_copy(*children, root);
	match_found = 1;
    }

    if(search_matches || match_found == 0) {
	xmlNode *child = NULL;
	for(child = __xml_first_child(root); child != NULL; child = __xml_next(child)) {
	    match_found += find_xml_children(
		children, child, tag, field, value,
		search_matches);
	}
    }
	
    return match_found;
}

gboolean
replace_xml_child(xmlNode *parent, xmlNode *child, xmlNode *update, gboolean delete_only)
{
    gboolean can_delete = FALSE;
    xmlNode *child_of_child = NULL;

    const char *up_id = NULL;
    const char *child_id = NULL;
    const char *right_val = NULL;
	
    CRM_CHECK(child != NULL, return FALSE);
    CRM_CHECK(update != NULL, return FALSE);

    up_id = ID(update);
    child_id = ID(child);
	
    if(up_id == NULL || safe_str_eq(child_id, up_id)) {
	can_delete = TRUE;
    } 
    if(safe_str_neq(crm_element_name(update), crm_element_name(child))) {
	can_delete = FALSE;
    }
    if(can_delete && delete_only) {
        xmlAttrPtr pIter = NULL;
        for(pIter = crm_first_attr(update); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = crm_attr_value(pIter);

            right_val = crm_element_value(child, p_name);
            if(safe_str_neq(p_value, right_val)) {
                can_delete = FALSE;
            }
        }
    }
	
    if(can_delete && parent != NULL) {
	crm_log_xml_trace(child, "Delete match found...");
	if(delete_only || update == NULL) {
	    free_xml(child);
		    
	} else {	
	    xmlNode *tmp = copy_xml(update);
	    xmlDoc *doc = tmp->doc;
	    xmlNode *old = xmlReplaceNode(child, tmp);
	    free_xml(old);
	    xmlDocSetRootElement(doc, NULL);
	    xmlFreeDoc(doc);
	}
	child = NULL;
	return TRUE;
		
    } else if(can_delete) {
	crm_log_xml_debug(child, "Cannot delete the search root");
	can_delete = FALSE;
    }

    child_of_child = __xml_first_child(child);
    while(child_of_child) {
	xmlNode *next = __xml_next(child_of_child);
	can_delete = replace_xml_child(child, child_of_child, update, delete_only);
	
	/* only delete the first one */
	if(can_delete) {
	    child_of_child = NULL;
	} else {
	    child_of_child = next;
	}
    }
	
    return can_delete;
}

void
hash2nvpair(gpointer key, gpointer value, gpointer user_data) 
{
    const char *name    = key;
    const char *s_value = value;

    xmlNode *xml_node  = user_data;
    xmlNode *xml_child = create_xml_node(xml_node, XML_CIB_TAG_NVPAIR);

    crm_xml_add(xml_child, XML_ATTR_ID, name);
    crm_xml_add(xml_child, XML_NVPAIR_ATTR_NAME, name);
    crm_xml_add(xml_child, XML_NVPAIR_ATTR_VALUE, s_value);

    crm_trace("dumped: name=%s value=%s", name, s_value);
}

void
hash2smartfield(gpointer key, gpointer value, gpointer user_data) 
{
    const char *name    = key;
    const char *s_value = value;

    xmlNode *xml_node  = user_data;

    if(isdigit(name[0])) {
	xmlNode *tmp = create_xml_node(xml_node, XML_TAG_PARAM);
	crm_xml_add(tmp, XML_NVPAIR_ATTR_NAME, name);
	crm_xml_add(tmp, XML_NVPAIR_ATTR_VALUE, s_value);
	    
    } else if(crm_element_value(xml_node, name) == NULL) {
	crm_xml_add(xml_node, name, s_value);
	crm_trace("dumped: %s=%s", name, s_value);

    } else {
	crm_trace("duplicate: %s=%s", name, s_value);
    }
}

void
hash2field(gpointer key, gpointer value, gpointer user_data) 
{
    const char *name    = key;
    const char *s_value = value;

    xmlNode *xml_node  = user_data;

    if(crm_element_value(xml_node, name) == NULL) {
	crm_xml_add(xml_node, name, s_value);
	crm_trace("dumped: %s=%s", name, s_value);

    } else {
	crm_trace("duplicate: %s=%s", name, s_value);
    }
}

void
hash2metafield(gpointer key, gpointer value, gpointer user_data) 
{
    char *crm_name = NULL;
    
    if(key == NULL || value == NULL) {
	return;
    } else if(((char*)key)[0] == '#') {
	return;
    } else if(strstr(key, ":")) {
	return;
    }
    
    crm_name = crm_meta_name(key);
    hash2field(crm_name, value, user_data);
    free(crm_name);
}


GHashTable *
xml2list(xmlNode *parent)
{
    xmlNode *child = NULL;
    xmlAttrPtr pIter = NULL;
    xmlNode *nvpair_list = NULL;
    GHashTable *nvpair_hash = g_hash_table_new_full(
	crm_str_hash, g_str_equal,
	g_hash_destroy_str, g_hash_destroy_str);
	
    CRM_CHECK(parent != NULL, return nvpair_hash);

    nvpair_list = find_xml_node(parent, XML_TAG_ATTRS, FALSE);
    if(nvpair_list == NULL) {
	crm_trace("No attributes in %s",
		    crm_element_name(parent));
	crm_log_xml_trace(
	    parent,"No attributes for resource op");
    }
	
    crm_log_xml_trace(nvpair_list, "Unpacking");

    for(pIter = crm_first_attr(nvpair_list); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;
	const char *p_value = crm_attr_value(pIter);
		
	crm_trace("Added %s=%s", p_name, p_value);
		
	g_hash_table_insert(nvpair_hash, strdup(p_name), strdup(p_value));
    }

    for(child = __xml_first_child(nvpair_list); child != NULL; child = __xml_next(child)) {
	if(crm_str_eq((const char *)child->name, XML_TAG_PARAM, TRUE)) {
	    const char *key = crm_element_value(child, XML_NVPAIR_ATTR_NAME);
	    const char *value = crm_element_value(child, XML_NVPAIR_ATTR_VALUE);
	    crm_trace("Added %s=%s", key, value);
	    if(key != NULL && value != NULL) {
		g_hash_table_insert(nvpair_hash, strdup(key), strdup(value));		
	    }
	}
    }
	
    return nvpair_hash;
}


typedef struct name_value_s 
{
	const char *name;
	const void *value;
} name_value_t;

static gint
sort_pairs(gconstpointer a, gconstpointer b)
{
    int rc = 0;
    const name_value_t *pair_a = a;
    const name_value_t *pair_b = b;

    CRM_ASSERT(a != NULL);
    CRM_ASSERT(pair_a->name != NULL);

    CRM_ASSERT(b != NULL);
    CRM_ASSERT(pair_b->name != NULL);

    rc = strcmp(pair_a->name, pair_b->name);
    if(rc < 0) {
	return -1;
    } else if(rc > 0) {
	return 1;
    }
    return 0;
}

static void
dump_pair(gpointer data, gpointer user_data)
{
    name_value_t *pair = data;
    xmlNode *parent = user_data;
    crm_xml_add(parent, pair->name, pair->value);
}

xmlNode *
sorted_xml(xmlNode *input, xmlNode *parent, gboolean recursive)
{
    xmlNode *child = NULL;
    GListPtr sorted = NULL;
    GListPtr unsorted = NULL;
    name_value_t *pair = NULL;
    xmlNode *result = NULL;
    const char *name = NULL;
    xmlAttrPtr pIter = NULL;

    CRM_CHECK(input != NULL, return NULL);
	
    name = crm_element_name(input);
    CRM_CHECK(name != NULL, return NULL);

    result = create_xml_node(parent, name);
	
    for(pIter = crm_first_attr(input); pIter != NULL; pIter = pIter->next) {
        const char *p_name = (const char *)pIter->name;
	const char *p_value = crm_attr_value(pIter);

        pair = calloc(1, sizeof(name_value_t));
        pair->name  = p_name;
        pair->value = p_value;
        unsorted = g_list_prepend(unsorted, pair);
        pair = NULL;
    }

    sorted = g_list_sort(unsorted, sort_pairs);
    g_list_foreach(sorted, dump_pair, result);
    g_list_free_full(sorted, free);

    for(child = __xml_first_child(input); child != NULL; child = __xml_next(child)) {
	if(recursive) {
	    sorted_xml(child, result, recursive);
	} else {
	    add_node_copy(result, child);
	}
    }
	
    return result;
}

static void
filter_xml(xmlNode *data, filter_t *filter, int filter_len, gboolean recursive) 
{
    int lpc = 0;
    xmlNode *child = NULL;
    
    for(lpc = 0; lpc < filter_len; lpc++) {
	xml_remove_prop(data, filter[lpc].string);
    }

    if(recursive == FALSE || filter_len == 0) {
	return;
    }
    
    for(child = __xml_first_child(data); child != NULL; child = __xml_next(child)) {
	filter_xml(child, filter, filter_len, recursive);
    }
}

/* "c048eae664dba840e1d2060f00299e9d" */
static char *
calculate_xml_digest_v1(xmlNode *input, gboolean sort, gboolean do_filter)
{
    char *digest = NULL;
    xmlNode *copy = NULL;
    char *buffer = NULL;

    if(sort || do_filter) {
	copy = sorted_xml(input, NULL, TRUE);
	input = copy;
    }

    if(do_filter) {
	filter_xml(input, filter, DIMOF(filter), TRUE);
    }

    buffer = dump_xml(input, FALSE, TRUE);
    CRM_CHECK(buffer != NULL && strlen(buffer) > 0, free_xml(copy); free(buffer); return NULL);

    digest = crm_md5sum(buffer);
    crm_trace("Digest %s: %s\n", digest, buffer);
    crm_log_xml_trace(copy,  "digest:source");

    free(buffer);
    free_xml(copy);
    return digest;
}

static char *
calculate_xml_digest_v2(xmlNode *source, gboolean do_filter)
{
    char *digest = NULL;

    int buffer_len = 0;
    int filter_size = DIMOF(filter);

    xmlDoc *doc = NULL;
    xmlNode *copy = NULL;
    xmlNode *input = source;
    xmlBuffer *xml_buffer = NULL;
    static struct qb_log_callsite *digest_cs = NULL;

    if(do_filter && BEST_EFFORT_STATUS) {
	/* Exclude the status calculation from the digest
	 *
	 * This doesn't mean it wont be sync'd, we just wont be paranoid
	 * about it being an _exact_ copy
	 *
	 * We don't need it to be exact, since we throw it away and regenerate
	 * from our peers whenever a new DC is elected anyway
	 *
	 * Importantly, this reduces the amount of XML to copy+export as 
	 * well as the amount of data for MD5 needs to operate on
	 */
	xmlNode *child = NULL;
        xmlAttrPtr pIter = NULL;
	copy = create_xml_node(NULL, XML_TAG_CIB);
        for(pIter = crm_first_attr(input); pIter != NULL; pIter = pIter->next) {
            const char *p_name = (const char *)pIter->name;
            const char *p_value = crm_attr_value(pIter);

            xmlSetProp(copy, (const xmlChar*)p_name, (const xmlChar*)p_value);
        }

	xml_remove_prop(copy, XML_ATTR_ORIGIN);
	xml_remove_prop(copy, XML_CIB_ATTR_WRITTEN);

	/* We just did all the filtering */
	
	for(child = __xml_first_child(input); child != NULL; child = __xml_next(child)) {
	    if(safe_str_neq(crm_element_name(child), XML_CIB_TAG_STATUS)) {
		add_node_copy(copy, child);
	    }
	}
	
    } else if(do_filter) {
	copy = copy_xml(input);
	filter_xml(copy, filter, filter_size, TRUE);
	input = copy;
    }

    doc = getDocPtr(input);
    xml_buffer = xmlBufferCreate();

    CRM_ASSERT(xml_buffer != NULL);
    CRM_CHECK(doc != NULL, return NULL); /* doc will only be NULL if an_xml_node is */
    
    buffer_len = xmlNodeDump(xml_buffer, doc, input, 0, FALSE);
    CRM_CHECK(xml_buffer->content != NULL && buffer_len > 0, goto done);

    digest = crm_md5sum((char *)xml_buffer->content);
    crm_trace("Digest %s\n", digest);

        if(digest_cs == NULL) {
            digest_cs = qb_log_callsite_get(__func__, __FILE__, "xml-blog", LOG_TRACE, __LINE__, 0);
        }
        if (digest_cs && digest_cs->targets) {
            char *trace_file = crm_concat("/tmp/cib-digest", digest, '-');
            crm_trace("Saving %s.%s.%s to %s",
                      crm_element_value(input, XML_ATTR_GENERATION_ADMIN),
                      crm_element_value(input, XML_ATTR_GENERATION),
                      crm_element_value(input, XML_ATTR_NUMUPDATES),
                      trace_file);
            save_xml_to_file(source, "digest input", trace_file);
            free(trace_file);
        }

  done:
    xmlBufferFree(xml_buffer);
    free_xml(copy);

    return digest;
}

char *
calculate_on_disk_digest(xmlNode *input)
{
    /* Always use the v1 format for on-disk digests
     * a) its a compatability nightmare
     * b) we only use this once at startup, all other
     *    invocations are in a separate child process 
     */
    return calculate_xml_digest_v1(input, FALSE, FALSE);
}

char *
calculate_operation_digest(xmlNode *input, const char *version)
{
    /* We still need the sorting for parameter digests */
    return calculate_xml_digest_v1(input, TRUE, FALSE);
}

char *
calculate_xml_digest(xmlNode *input, gboolean sort, gboolean do_filter)
{
    return calculate_xml_digest_v1(input, sort, do_filter);
}

char *
calculate_xml_versioned_digest(xmlNode *input, gboolean sort, gboolean do_filter, const char *version)
{
    /*
     * The sorting associated with v1 digest creation accounted for 23% of
     * the CIB's CPU usage on the server. v2 drops this.
     *
     * The filtering accounts for an additional 2.5% and we may want to
     * remove it in future.
     *
     * v2 also uses the xmlBuffer contents directly to avoid additional copying
     */
    if(version == NULL || compare_version("3.0.5", version) > 0) {
	crm_trace("Using v1 digest algorithm for %s", crm_str(version));
	return calculate_xml_digest_v1(input, sort, do_filter);
    }
    crm_trace("Using v2 digest algorithm for %s", crm_str(version));
    return calculate_xml_digest_v2(input, do_filter);
}

static gboolean
validate_with_dtd(
    xmlDocPtr doc, gboolean to_logs, const char *dtd_file) 
{
    gboolean valid = TRUE;

    xmlDtdPtr dtd = NULL;
    xmlValidCtxtPtr cvp = NULL;
	
    CRM_CHECK(doc != NULL, return FALSE);
    CRM_CHECK(dtd_file != NULL, return FALSE);

    dtd = xmlParseDTD(NULL, (const xmlChar *)dtd_file);
    CRM_CHECK(dtd != NULL, crm_err("Could not find/parse %s", dtd_file); goto cleanup);

    cvp = xmlNewValidCtxt();
    CRM_CHECK(cvp != NULL, goto cleanup);

    if(to_logs) {
	cvp->userData = (void *) LOG_ERR;
	cvp->error    = (xmlValidityErrorFunc) xml_log;
	cvp->warning  = (xmlValidityWarningFunc) xml_log;
    } else {
	cvp->userData = (void *) stderr;
	cvp->error    = (xmlValidityErrorFunc) fprintf;
	cvp->warning  = (xmlValidityWarningFunc) fprintf;
    }
	
    if (!xmlValidateDtd(cvp, doc, dtd)) {
	valid = FALSE;
    }
	
  cleanup:
    if(cvp) {
	xmlFreeValidCtxt(cvp);
    }
    if(dtd) {
	xmlFreeDtd(dtd);
    }
	
    return valid;
}

xmlNode *first_named_child(xmlNode *parent, const char *name) 
{
    xmlNode *match = NULL;
    for(match = __xml_first_child(parent); match != NULL; match = __xml_next(match)) {
	/*
	 * name == NULL gives first child regardless of name; this is
	 * semantically incorrect in this funciton, but may be necessary
	 * due to prior use of xml_child_iter_filter
	 */
	if(name == NULL || crm_str_eq((const char*)match->name, name, TRUE)) {
		return match;
	}
    }
    return NULL;
}

#if 0
static void relaxng_invalid_stderr(void * userData, xmlErrorPtr error)
{
    /*
      Structure xmlError
      struct _xmlError {
      int	domain	: What part of the library raised this er
      int	code	: The error code, e.g. an xmlParserError
      char *	message	: human-readable informative error messag
      xmlErrorLevel	level	: how consequent is the error
      char *	file	: the filename
      int	line	: the line number if available
      char *	str1	: extra string information
      char *	str2	: extra string information
      char *	str3	: extra string information
      int	int1	: extra number information
      int	int2	: column number of the error or 0 if N/A
      void *	ctxt	: the parser context if available
      void *	node	: the node in the tree
      }
    */
    crm_err("Structured error: line=%d, level=%d %s",
	    error->line, error->level, error->message);
}
#endif

static gboolean
validate_with_relaxng(
    xmlDocPtr doc, gboolean to_logs, const char *relaxng_file, relaxng_ctx_cache_t **cached_ctx) 
{
    int rc = 0;
    gboolean valid = TRUE;
    relaxng_ctx_cache_t *ctx = NULL;
    
    CRM_CHECK(doc != NULL, return FALSE);
    CRM_CHECK(relaxng_file != NULL, return FALSE);

    
    if(cached_ctx && *cached_ctx) {
	ctx = *cached_ctx;

    } else {
	crm_info("Creating RNG parser context");
	ctx = calloc(1, sizeof(relaxng_ctx_cache_t));
	
	xmlLoadExtDtdDefaultValue = 1;
	ctx->parser = xmlRelaxNGNewParserCtxt(relaxng_file);
	CRM_CHECK(ctx->parser != NULL, goto cleanup);

	if(to_logs) {
	    xmlRelaxNGSetParserErrors(ctx->parser,
				      (xmlRelaxNGValidityErrorFunc) xml_log,
				      (xmlRelaxNGValidityWarningFunc) xml_log,
				      GUINT_TO_POINTER(LOG_ERR));
	} else {
	    xmlRelaxNGSetParserErrors(ctx->parser,
				      (xmlRelaxNGValidityErrorFunc) fprintf,
				      (xmlRelaxNGValidityWarningFunc) fprintf,
				      stderr);
	}

	ctx->rng = xmlRelaxNGParse(ctx->parser);
	CRM_CHECK(ctx->rng != NULL, crm_err("Could not find/parse %s", relaxng_file); goto cleanup);

	ctx->valid = xmlRelaxNGNewValidCtxt(ctx->rng);
	CRM_CHECK(ctx->valid != NULL, goto cleanup);

	if(to_logs) {
	    xmlRelaxNGSetValidErrors(ctx->valid,
				     (xmlRelaxNGValidityErrorFunc) xml_log,
				     (xmlRelaxNGValidityWarningFunc) xml_log,
				     GUINT_TO_POINTER(LOG_ERR));
	} else {
	    xmlRelaxNGSetValidErrors(ctx->valid,
				     (xmlRelaxNGValidityErrorFunc) fprintf,
				     (xmlRelaxNGValidityWarningFunc) fprintf,
				     stderr);
	}
    }
    
    /* xmlRelaxNGSetValidStructuredErrors( */
    /* 	valid, relaxng_invalid_stderr, valid); */
    
    xmlLineNumbersDefault(1);
    rc = xmlRelaxNGValidateDoc(ctx->valid, doc);
    if (rc > 0) {
	valid = FALSE;

    } else if (rc < 0) {
	crm_err("Internal libxml error during validation\n");
    }

  cleanup:

    if(cached_ctx) {
	*cached_ctx = ctx;

    } else {
	if(ctx->parser != NULL) {
	    xmlRelaxNGFreeParserCtxt(ctx->parser);
	}
	if(ctx->valid != NULL) {
	    xmlRelaxNGFreeValidCtxt(ctx->valid);
	} 
	if (ctx->rng != NULL) {
	    xmlRelaxNGFree(ctx->rng);    
	}
	free(ctx);
    }
    
    return valid;
}

void crm_xml_cleanup(void)
{
    int lpc = 0;
    relaxng_ctx_cache_t *ctx = NULL;

    crm_info("Cleaning up memory from libxml2");
    for(; lpc < all_schemas; lpc++) {
	switch(known_schemas[lpc].type) {
	    case 0:
		/* None */
		break;
	    case 1:
		/* DTD - Not cached */
		break;
	    case 2:
		/* RNG - Cached */
		ctx = (relaxng_ctx_cache_t *)known_schemas[lpc].cache;
		if(ctx == NULL) {
		    break;
		}
		if(ctx->parser != NULL) {
		    xmlRelaxNGFreeParserCtxt(ctx->parser);
		}
		if(ctx->valid != NULL) {
		    xmlRelaxNGFreeValidCtxt(ctx->valid);
		} 
		if (ctx->rng != NULL) {
		    xmlRelaxNGFree(ctx->rng);    
		}
		free(ctx);
		known_schemas[lpc].cache = NULL;
		break;
	    default:
		break;
	}
    }
    xmlCleanupParser();
}

static gboolean validate_with(xmlNode *xml, int method, gboolean to_logs) 
{
    xmlDocPtr doc = NULL;
    gboolean valid = FALSE;
    int type = known_schemas[method].type;
    char *file = NULL;

    CRM_CHECK(xml != NULL, return FALSE);
    doc = getDocPtr(xml);
    file = get_schema_path(known_schemas[method].location);
    
    crm_trace("Validating with: %s (type=%d)", crm_str(file), type);
    switch(type) {
	case 0:
	    valid = TRUE;
	    break;
	case 1:
	    valid = validate_with_dtd(doc, to_logs, file);
	    break;
	case 2:
	    valid = validate_with_relaxng(doc, to_logs, file, (relaxng_ctx_cache_t**)&(known_schemas[method].cache));
	    break;
	default:
	    crm_err("Unknown validator type: %d", type);
	    break;
    }

    free(file);
    return valid;
}

#include <stdio.h>
static void dump_file(const char *filename) 
{

    FILE *fp = NULL;
    int ch, line = 0;

    CRM_CHECK(filename != NULL, return);

    fp = fopen(filename, "r");
    CRM_CHECK(fp != NULL, return);

    fprintf(stderr, "%4d ", ++line);
    do {
	ch = getc(fp);
	if(ch == EOF) {
	    putc('\n', stderr);
	    break;
	} else if(ch == '\n') {
	    fprintf(stderr, "\n%4d ", ++line);
	} else {
	    putc(ch, stderr);
	}
    } while(1);
    
    fclose(fp);
}

gboolean validate_xml_verbose(xmlNode *xml_blob) 
{
    xmlDoc *doc = NULL;
    xmlNode *xml = NULL;
    gboolean rc = FALSE;

    char *filename = NULL;
    static char *template = NULL;
    if(template == NULL) {
	template = strdup(CRM_STATE_DIR"/cib-invalid.XXXXXX");
    }
    
    filename = mktemp(template);
    write_xml_file(xml_blob, filename, FALSE);
    
    dump_file(filename);
    
    doc = xmlParseFile(filename);
    xml = xmlDocGetRootElement(doc);
    rc = validate_xml(xml, NULL, FALSE);
    free_xml(xml);
    
    return rc;
}

gboolean validate_xml(xmlNode *xml_blob, const char *validation, gboolean to_logs)
{
    int lpc = 0;
    
    if(validation == NULL) {
	validation = crm_element_value(xml_blob, XML_ATTR_VALIDATION);
    }

    if(validation == NULL) {
	validation = crm_element_value(xml_blob, "ignore-dtd");
	if(crm_is_true(validation)) {
	    validation = "none";
	} else {
	    validation = "pacemaker-1.0";
	}
    }
    
    if(safe_str_eq(validation, "none")) {
	return TRUE;
    }
    
    for(; lpc < all_schemas; lpc++) {
	if(safe_str_eq(validation, known_schemas[lpc].name)) {
	    return validate_with(xml_blob, lpc, to_logs);
	}
    }

    crm_err("Unknown validator: %s", validation);
    return FALSE;
}

#if HAVE_LIBXSLT
static xmlNode *apply_transformation(xmlNode *xml, const char *transform) 
{
    char *xform = NULL;
    xmlNode *out = NULL;
    xmlDocPtr res = NULL;
    xmlDocPtr doc = NULL;
    xsltStylesheet *xslt = NULL;

    CRM_CHECK(xml != NULL, return FALSE);
    doc = getDocPtr(xml);
    xform = get_schema_path(transform);

    xmlLoadExtDtdDefaultValue = 1;
    xmlSubstituteEntitiesDefault(1);
    
    xslt = xsltParseStylesheetFile((const xmlChar *)xform);
    CRM_CHECK(xslt != NULL, goto cleanup);
    
    res = xsltApplyStylesheet(xslt, doc, NULL);
    CRM_CHECK(res != NULL, goto cleanup);

    out = xmlDocGetRootElement(res);
    
  cleanup:
    if(xslt) {
	xsltFreeStylesheet(xslt);
    }

    xsltCleanupGlobals();
    xmlCleanupParser();
    free(xform);

    return out;
}
#endif

const char *get_schema_name(int version)
{
    if(version < 0 || version >= all_schemas) {
	return "unknown";
    }
    return known_schemas[version].name;
}


int get_schema_version(const char *name) 
{
    int lpc = 0;
    for(; lpc < all_schemas; lpc++) {
	if(safe_str_eq(name, known_schemas[lpc].name)) {
	    return lpc;
	}
    }
    return -1;
}

/* set which validation to use */
#include <crm/cib.h>
int update_validation(
    xmlNode **xml_blob, int *best, gboolean transform, gboolean to_logs) 
{
    xmlNode *xml = NULL;
    char *value = NULL;
    int lpc = 0, match = -1, rc = pcmk_ok;

    CRM_CHECK(best != NULL, return -EINVAL);
    CRM_CHECK(xml_blob != NULL, return -EINVAL);
    CRM_CHECK(*xml_blob != NULL, return -EINVAL);
    
    *best = 0;
    xml = *xml_blob;
    value = crm_element_value_copy(xml, XML_ATTR_VALIDATION);

    if(value != NULL) {
	match = get_schema_version(value);
	
	lpc = match;
	if(lpc >= 0 && transform == FALSE) {
	    lpc++;

	} else if(lpc < 0) {
	    crm_debug("Unknown validation type");
	    lpc = 0;
	}
    }

    if(match >= max_schemas) {
	/* nothing to do */
	free(value);
	*best = match;
	return pcmk_ok;
    }
    
    for(; lpc < max_schemas; lpc++) {
	gboolean valid = TRUE;
	crm_debug("Testing '%s' validation", known_schemas[lpc].name?known_schemas[lpc].name:"<unset>");
	valid = validate_with(xml, lpc, to_logs);
	
	if(valid) {
	    *best = lpc;
	}
	
	if(valid && transform) {
	    xmlNode *upgrade = NULL;
	    int next = known_schemas[lpc].after_transform;
	    if(next <= 0) {
		next = lpc+1;
	    }
	    
	    crm_notice("Upgrading %s-style configuration to %s with %s",
		       known_schemas[lpc].name, known_schemas[next].name, known_schemas[lpc].transform?known_schemas[lpc].transform:"no-op");

	    if(known_schemas[lpc].transform == NULL) {
		if(validate_with(xml, next, to_logs)) {
		    crm_debug("Configuration valid for schema: %s", known_schemas[next].name);
		    lpc = next; *best = next;
		    rc = pcmk_ok;

		} else {
		    crm_info("Configuration not valid for schema: %s", known_schemas[next].name);
		}
		
	    } else {
#if HAVE_LIBXSLT
		upgrade = apply_transformation(xml, known_schemas[lpc].transform);
#endif
		if(upgrade == NULL) {
		    crm_err("Transformation %s failed", known_schemas[lpc].transform);
		    rc = -pcmk_err_transform_failed;
		    
		} else if(validate_with(upgrade, next, to_logs)) {
		    crm_info("Transformation %s successful", known_schemas[lpc].transform);
		    lpc = next; *best = next;
		    free_xml(xml);
		    xml = upgrade;
		    rc = pcmk_ok;
		    
		} else {
		    crm_err("Transformation %s did not produce a valid configuration", known_schemas[lpc].transform);
		    crm_log_xml_info(upgrade, "transform:bad");
		    free_xml(upgrade);
		    rc = -pcmk_err_dtd_validation;
		}
	    }
	}
    }
    
    if(*best > match) {
	crm_notice("Upgraded from %s to %s validation", value?value:"<none>", known_schemas[*best].name);
	crm_xml_add(xml, XML_ATTR_VALIDATION, known_schemas[*best].name);
    }

    *xml_blob = xml;
    free(value);
    return rc;
}

xmlNode *
getXpathResult(xmlXPathObjectPtr xpathObj, int index) 
{
    xmlNode *match = NULL;
    CRM_CHECK(index >= 0, return NULL);
    CRM_CHECK(xpathObj != NULL, return NULL);

    if(index >= xpathObj->nodesetval->nodeNr) {
	crm_err("Requested index %d of only %d items", index, xpathObj->nodesetval->nodeNr);
	return NULL;
    }
    
    match = xpathObj->nodesetval->nodeTab[index];
    CRM_CHECK(match != NULL, return NULL);

    /*
     * From xpath2.c
     *
     * All the elements returned by an XPath query are pointers to
     * elements from the tree *except* namespace nodes where the XPath
     * semantic is different from the implementation in libxml2 tree.
     * As a result when a returned node set is freed when
     * xmlXPathFreeObject() is called, that routine must check the
     * element type. But node from the returned set may have been removed
     * by xmlNodeSetContent() resulting in access to freed data.
     * This can be exercised by running
     *       valgrind xpath2 test3.xml '//discarded' discarded
     * There is 2 ways around it:
     *   - make a copy of the pointers to the nodes from the result set 
     *     then call xmlXPathFreeObject() and then modify the nodes
     * or
     *   - remove the reference to the modified nodes from the node set
     *     as they are processed, if they are not namespace nodes.
     */
    if (xpathObj->nodesetval->nodeTab[index]->type != XML_NAMESPACE_DECL) {
	xpathObj->nodesetval->nodeTab[index] = NULL;
    }

    if(match->type == XML_DOCUMENT_NODE) {
	/* Will happen if section = '/' */
	match = match->children;

    } else if(match->type != XML_ELEMENT_NODE
	      && match->parent
	      && match->parent->type == XML_ELEMENT_NODE) {
	/* reurning the parent instead */
	match = match->parent;
	
    } else if(match->type != XML_ELEMENT_NODE) {
	/* We only support searching nodes */
	crm_err("We only support %d not %d", XML_ELEMENT_NODE, match->type);
	match = NULL;
    }
    return match;
}

/* the caller needs to check if the result contains a xmlDocPtr or xmlNodePtr */
xmlXPathObjectPtr 
xpath_search(xmlNode *xml_top, const char *path)
{
    xmlDocPtr doc = NULL;
    xmlXPathObjectPtr xpathObj = NULL; 
    xmlXPathContextPtr xpathCtx = NULL; 
    const xmlChar *xpathExpr = (const xmlChar *)path;

    CRM_CHECK(path != NULL, return NULL);
    CRM_CHECK(xml_top != NULL, return NULL);
    CRM_CHECK(strlen(path) > 0, return NULL);
    
    doc = getDocPtr(xml_top);

    crm_trace("Evaluating: %s", path);
    xpathCtx = xmlXPathNewContext(doc);
    CRM_ASSERT(xpathCtx != NULL);
    
    xpathObj = xmlXPathEvalExpression(xpathExpr, xpathCtx);
    xmlXPathFreeContext(xpathCtx);
    return xpathObj;
}

gboolean
cli_config_update(xmlNode **xml, int *best_version, gboolean to_logs) 
{
    gboolean rc = TRUE;
    static int min_version = -1;
    static int max_version = -1;

    const char *value = crm_element_value(*xml, XML_ATTR_VALIDATION);
    int version = get_schema_version(value);

    if(min_version < 0) {
	min_version = get_schema_version(MINIMUM_SCHEMA_VERSION);
    }
    if(max_version < 0) {
	max_version = get_schema_version(LATEST_SCHEMA_VERSION);
    }
    
    if(version < min_version) {
	xmlNode *converted = NULL;
	
	converted = copy_xml(*xml);
	update_validation(&converted, &version, TRUE, to_logs);
	
	value = crm_element_value(converted, XML_ATTR_VALIDATION);
	if(version < min_version) {
	    if(to_logs) {
		crm_config_err("Your current configuration could only be upgraded to %s... "
			       "the minimum requirement is %s.\n", crm_str(value), MINIMUM_SCHEMA_VERSION);
	    } else {
		fprintf(stderr, "Your current configuration could only be upgraded to %s... "
			"the minimum requirement is %s.\n", crm_str(value), MINIMUM_SCHEMA_VERSION);
	    }
	    
	    free_xml(converted);
	    converted = NULL;
	    rc = FALSE;
	    
	} else {
	    free_xml(*xml);
	    *xml = converted;

	    if(version < max_version) {
		crm_config_warn("Your configuration was internally updated to %s... "
				"which is acceptable but not the most recent",
				get_schema_name(version));
		
	    } else if(to_logs){
		crm_info("Your configuration was internally updated to the latest version (%s)",
			 get_schema_name(version));
	    } 
	}
    } else if(version > max_version) {
	if(to_logs){
	    crm_config_warn("Configuration validation is currently disabled."
			    " It is highly encouraged and prevents many common cluster issues.");

	} else {
	    fprintf(stderr, "Configuration validation is currently disabled."
		    " It is highly encouraged and prevents many common cluster issues.\n");
	}
    }

    if(best_version) {
	*best_version = version;	    
    }
    
    return rc;
}

xmlNode *expand_idref(xmlNode *input, xmlNode *top) 
{
    const char *tag = NULL;
    const char *ref = NULL;
    xmlNode *result = input;
    char *xpath_string = NULL;

    if(result == NULL) {
	return NULL;

    } else if(top == NULL) {
	top = input;
    }

    tag = crm_element_name(result);
    ref = crm_element_value(result, XML_ATTR_IDREF);
    
    if(ref != NULL) {
	int xpath_max = 512, offset = 0;
	xpath_string = calloc(1, xpath_max);

	offset += snprintf(xpath_string + offset, xpath_max - offset, "//%s[@id='%s']", tag, ref);
	result = get_xpath_object(xpath_string, top, LOG_ERR);
	if(result == NULL) {
	    char *nodePath = (char *)xmlGetNodePath(top);
	    crm_err("No match for %s found in %s: Invalid configuration", xpath_string, crm_str(nodePath));
	    free(nodePath);
	}
    }
    
    free(xpath_string);
    return result;
}

xmlNode*
get_xpath_object_relative(const char *xpath, xmlNode *xml_obj, int error_level)
{
    int len = 0;
    xmlNode *result = NULL;
    char *xpath_full = NULL;
    char *xpath_prefix = NULL;
    
    if(xml_obj == NULL || xpath == NULL) {
	return NULL;
    }

    xpath_prefix = (char *)xmlGetNodePath(xml_obj);
    len += strlen(xpath_prefix);
    len += strlen(xpath);

    xpath_full = strdup(xpath_prefix);
    xpath_full = realloc(xpath_full, len+1);
    strncat(xpath_full, xpath, len);

    result = get_xpath_object(xpath_full, xml_obj, error_level);

    free(xpath_prefix);
    free(xpath_full);
    return result;
}

xmlNode*
get_xpath_object(const char *xpath, xmlNode *xml_obj, int error_level)
{
    xmlNode *result = NULL;
    xmlXPathObjectPtr xpathObj = NULL;
    char *nodePath = NULL;
    char *matchNodePath = NULL;

    if(xpath == NULL) {
	return xml_obj; /* or return NULL? */
    }
    
    xpathObj = xpath_search(xml_obj, xpath);
    nodePath = (char *)xmlGetNodePath(xml_obj);
    if(xpathObj == NULL || xpathObj->nodesetval == NULL || xpathObj->nodesetval->nodeNr < 1) {
	do_crm_log(error_level, "No match for %s in %s", xpath, crm_str(nodePath));
	crm_log_xml_trace(xml_obj, "Unexpected Input");
	
    } else if(xpathObj->nodesetval->nodeNr > 1) {
	int lpc = 0, max = xpathObj->nodesetval->nodeNr;
	do_crm_log(error_level, "Too many matches for %s in %s", xpath, crm_str(nodePath));

	for(lpc = 0; lpc < max; lpc++) {
	    xmlNode *match = getXpathResult(xpathObj, lpc);
	    CRM_CHECK(match != NULL, continue);

	    matchNodePath = (char *)xmlGetNodePath(match);
	    do_crm_log(error_level, "%s[%d] = %s", xpath, lpc, crm_str(matchNodePath));
	    free(matchNodePath);
	}
	crm_log_xml_trace(xml_obj, "Bad Input");

    } else {
	result = getXpathResult(xpathObj, 0);
    }
    
    if(xpathObj) {
	xmlXPathFreeObject(xpathObj);
    }
    free(nodePath);

    return result;
}

const char *
crm_element_value(xmlNode *data, const char *name)
{
    xmlAttr *attr = NULL;
    
    if(data == NULL) {
	crm_err("Couldn't find %s in NULL", name?name:"<null>");
        CRM_LOG_ASSERT(data != NULL);
	return NULL;

    } else if(name == NULL) {
	crm_err("Couldn't find NULL in %s", crm_element_name(data));
	return NULL;
    }
    
    attr = xmlHasProp(data, (const xmlChar*)name);
    if(attr == NULL || attr->children == NULL) {
	return NULL;
    }
    return (const char*)attr->children->content;
}

