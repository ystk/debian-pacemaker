
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

#include <crm_internal.h>
#include <crm/crm.h>

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <glib.h>

#include <crm/common/xml.h>
#include <crm/common/util.h>
#include <crm/msg_xml.h>
#include <crm/cib.h>
#include <crm/pengine/status.h>

gboolean USE_LIVE_CIB = FALSE;
char *cib_save = NULL;
void usage(const char *cmd, int exit_status);
extern gboolean stage0(pe_working_set_t * data_set);
extern void cleanup_alloc_calculations(pe_working_set_t * data_set);
extern xmlNode *do_calculations(pe_working_set_t * data_set, xmlNode * xml_input, crm_time_t * now);

/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    /* Top-level Options */
    {"help",           0, 0, '?', "\tThis text"},
    {"version",        0, 0, '$', "\tVersion information"  },
    {"verbose",        0, 0, 'V', "\tIncrease debug output\n"},
    
    {"-spacer-",	1, 0, '-', "\nData sources:"},
    {"live-check",  0, 0, 'L', "Check the configuration used by the running cluster\n"},
    {"xml-file",    1, 0, 'x', "Check the configuration in the named file"},
    {"xml-text",    1, 0, 'X', "Check the configuration in the supplied string"},
    {"xml-pipe",    0, 0, 'p', "Check the configuration piped in via stdin"},

    {"-spacer-",    1, 0, '-', "\nAdditional Options:"},
    {"save-xml",    1, 0, 'S', "Save the verified XML to the named file.  Most useful with -L"},

    {"-spacer-",    1, 0, '-', "\nExamples:", pcmk_option_paragraph},
    {"-spacer-",    1, 0, '-', "Check the consistency of the configuration in the running cluster:", pcmk_option_paragraph},
    {"-spacer-",    1, 0, '-', " crm_verify --live-check", pcmk_option_example},
    {"-spacer-",    1, 0, '-', "Check the consistency of the configuration in a given file and produce verbose output:", pcmk_option_paragraph},
    {"-spacer-",    1, 0, '-', " crm_verify --xml-file file.xml --verbose", pcmk_option_example},
  
    {F_CRM_DATA,    1, 0, 'X', NULL, 1}, /* legacy */
    {0, 0, 0, 0}
};
/* *INDENT-ON* */

int
main(int argc, char **argv)
{
    xmlNode *cib_object = NULL;
    xmlNode *status = NULL;
    int argerr = 0;
    int flag;
    int option_index = 0;

    pe_working_set_t data_set;
    cib_t *cib_conn = NULL;
    int rc = pcmk_ok;

    bool verbose = FALSE;
    gboolean xml_stdin = FALSE;
    const char *xml_tag = NULL;
    const char *xml_file = NULL;
    const char *xml_string = NULL;

    crm_log_cli_init("crm_verify");
    crm_set_options(NULL, "[modifiers] data_source", long_options,
                    "Check a (complete) confiuration for syntax and common conceptual errors."
                    "\n\nChecks the well-formedness of an XML configuration, its conformance to the configured DTD/schema and for the presence of common misconfigurations."
                    "\n\nIt reports two classes of problems, errors and warnings."
                    " Errors must be fixed before the cluster will work properly."
                    " However, it is left up to the administrator to decide if the warnings should also be fixed.");

    while (1) {
        flag = crm_get_option(argc, argv, &option_index);
        if (flag == -1)
            break;

        switch (flag) {
            case 'X':
                crm_trace("Option %c => %s", flag, optarg);
                xml_string = optarg;
                break;
            case 'x':
                crm_trace("Option %c => %s", flag, optarg);
                xml_file = optarg;
                break;
            case 'p':
                xml_stdin = TRUE;
                break;
            case 'S':
                cib_save = optarg;
                break;
            case 'V':
                verbose = TRUE;
                crm_bump_log_level(argc, argv);
                break;
            case 'L':
                USE_LIVE_CIB = TRUE;
                break;
            case '$':
            case '?':
                crm_help(flag, EX_OK);
                break;
            default:
                fprintf(stderr, "Option -%c is not yet supported\n", flag);
                ++argerr;
                break;
        }
    }

    if (optind < argc) {
        printf("non-option ARGV-elements: ");
        while (optind < argc) {
            printf("%s ", argv[optind++]);
        }
        printf("\n");
    }

    if (optind > argc) {
        ++argerr;
    }

    if (argerr) {
        crm_err("%d errors in option parsing", argerr);
        crm_help(flag, EX_USAGE);
    }

    crm_info("=#=#=#=#= Getting XML =#=#=#=#=");

    if (USE_LIVE_CIB) {
        cib_conn = cib_new();
        rc = cib_conn->cmds->signon(cib_conn, crm_system_name, cib_command);
    }

    if (USE_LIVE_CIB) {
        if (rc == pcmk_ok) {
            int options = cib_scope_local | cib_sync_call;

            crm_info("Reading XML from: live cluster");
            rc = cib_conn->cmds->query(cib_conn, NULL, &cib_object, options);
        }

        if (rc != pcmk_ok) {
            fprintf(stderr, "Live CIB query failed: %s\n", pcmk_strerror(rc));
            goto done;
        }
        if (cib_object == NULL) {
            fprintf(stderr, "Live CIB query failed: empty result\n");
            rc = -ENOMSG;
            goto done;
        }

    } else if (xml_file != NULL) {
        cib_object = filename2xml(xml_file);
        if (cib_object == NULL) {
            fprintf(stderr, "Couldn't parse input file: %s\n", xml_file);
            rc = -ENODATA;
            goto done;
        }

    } else if (xml_string != NULL) {
        cib_object = string2xml(xml_string);
        if (cib_object == NULL) {
            fprintf(stderr, "Couldn't parse input string: %s\n", xml_string);
            rc = -ENODATA;
            goto done;
        }
    } else if (xml_stdin) {
        cib_object = stdin2xml();
        if (cib_object == NULL) {
            fprintf(stderr, "Couldn't parse input from STDIN.\n");
            rc = -ENODATA;
            goto done;
        }

    } else {
        fprintf(stderr, "No configuration source specified."
                "  Use --help for usage information.\n");
        rc = -ENODATA;
        goto done;
    }

    xml_tag = crm_element_name(cib_object);
    if (safe_str_neq(xml_tag, XML_TAG_CIB)) {
        fprintf(stderr,
                "This tool can only check complete configurations (ie. those starting with <cib>).\n");
        rc = -EBADMSG;
        goto done;
    }

    if (cib_save != NULL) {
        write_xml_file(cib_object, cib_save, FALSE);
    }

    status = get_object_root(XML_CIB_TAG_STATUS, cib_object);
    if (status == NULL) {
        create_xml_node(cib_object, XML_CIB_TAG_STATUS);
    }

    if (validate_xml(cib_object, NULL, FALSE) == FALSE) {
        crm_config_err("CIB did not pass DTD/schema validation");
        free_xml(cib_object);
        cib_object = NULL;

    } else if (cli_config_update(&cib_object, NULL, FALSE) == FALSE) {
        crm_config_error = TRUE;
        free_xml(cib_object);
        cib_object = NULL;
        fprintf(stderr, "The cluster will NOT be able to use this configuration.\n");
        fprintf(stderr, "Please manually update the configuration to conform to the %s syntax.\n",
                xml_latest_schema());
    }

    set_working_set_defaults(&data_set);
    if (cib_object == NULL) {
    } else if (status != NULL || USE_LIVE_CIB) {
        /* live queries will always have a status section and can do a full simulation */
        do_calculations(&data_set, cib_object, NULL);
        cleanup_alloc_calculations(&data_set);

    } else {
        data_set.now = crm_time_new(NULL);
        data_set.input = cib_object;
        stage0(&data_set);
        cleanup_alloc_calculations(&data_set);
    }

    if (crm_config_error) {
        fprintf(stderr, "Errors found during check: config not valid\n");
        if (verbose == FALSE) {
            fprintf(stderr, "  -V may provide more details\n");
        }
        rc = -pcmk_err_generic;

    } else if (crm_config_warning) {
        fprintf(stderr, "Warnings found during check: config may not be valid\n");
        if (verbose == FALSE) {
            fprintf(stderr, "  Use -V for more details\n");
        }
        rc = -pcmk_err_generic;
    }

    if (USE_LIVE_CIB && cib_conn) {
        cib_conn->cmds->signoff(cib_conn);
        cib_delete(cib_conn);
    }

  done:
    return rc;
}
