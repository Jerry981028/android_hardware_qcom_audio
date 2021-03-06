/*
 * Copyright (c) 2014-2017, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "platform_info"
#define LOG_NDDEBUG 0

#include <errno.h>
#include <stdio.h>
#include <expat.h>
#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <audio_hw.h>
#include "acdb.h"
#include "platform_api.h"
#include <platform.h>
#include <math.h>

#ifdef DYNAMIC_LOG_ENABLED
#include <log_xml_parser.h>
#define LOG_MASK HAL_MOD_FILE_PLATFORM_INFO
#include <log_utils.h>
#endif

#define BUF_SIZE                    1024

typedef enum {
    ROOT,
    ACDB,
    BITWIDTH,
    PCM_ID,
    BACKEND_NAME,
    INTERFACE_NAME,
    CONFIG_PARAMS,
    GAIN_LEVEL_MAPPING,
    ACDB_METAINFO_KEY,
    DEVICE_NAME,
} section_t;

typedef void (* section_process_fn)(const XML_Char **attr);

static void process_acdb_id(const XML_Char **attr);
static void process_bit_width(const XML_Char **attr);
static void process_pcm_id(const XML_Char **attr);
static void process_backend_name(const XML_Char **attr);
static void process_interface_name(const XML_Char **attr);
static void process_config_params(const XML_Char **attr);
static void process_root(const XML_Char **attr);
static void process_gain_db_to_level_map(const XML_Char **attr);
static void process_acdb_metainfo_key(const XML_Char **attr);
static void process_device_name(const XML_Char **attr);

static section_process_fn section_table[] = {
    [ROOT] = process_root,
    [ACDB] = process_acdb_id,
    [BITWIDTH] = process_bit_width,
    [PCM_ID] = process_pcm_id,
    [BACKEND_NAME] = process_backend_name,
    [INTERFACE_NAME] = process_interface_name,
    [CONFIG_PARAMS] = process_config_params,
    [GAIN_LEVEL_MAPPING] = process_gain_db_to_level_map,
    [ACDB_METAINFO_KEY] = process_acdb_metainfo_key,
    [DEVICE_NAME] = process_device_name,
};

static section_t section;

struct platform_info {
    caller_t          caller;
    void             *platform;
    struct str_parms *kvpairs;
};

static struct platform_info my_data;

/*
 * <audio_platform_info>
 * <acdb_ids>
 * <device name="???" acdb_id="???"/>
 * ...
 * ...
 * </acdb_ids>
 * <backend_names>
 * <device name="???" backend="???"/>
 * ...
 * ...
 * </backend_names>
 * <pcm_ids>
 * <usecase name="???" type="in/out" id="???"/>
 * ...
 * ...
 * </pcm_ids>
 * <interface_names>
 * <device name="Use audio device name here, not sound device name" interface="PRIMARY_I2S" codec_type="external/internal"/>
 * ...
 * ...
 * </interface_names>
 * <config_params>
 *      <param key="snd_card_name" value="msm8994-tomtom-mtp-snd-card"/>
 *      ...
 *      ...
 * </config_params>
 * <device_names>
 * <device name="???" alias="???"/>
 * ...
 * ...
 * </device_names>
 * </audio_platform_info>
 */

static void process_root(const XML_Char **attr __unused)
{
}

/* mapping from usecase to pcm dev id */
static void process_pcm_id(const XML_Char **attr)
{
    int index;

    if (strcmp(attr[0], "name") != 0) {
        ALOGE("%s: 'name' not found, no pcm_id set!", __func__);
        goto done;
    }

    index = platform_get_usecase_index((char *)attr[1]);
    if (index < 0) {
        ALOGE("%s: usecase %s not found!",
              __func__, attr[1]);
        goto done;
    }

    if (strcmp(attr[2], "type") != 0) {
        ALOGE("%s: usecase type not mentioned", __func__);
        goto done;
    }

    int type = -1;

    if (!strcasecmp((char *)attr[3], "in")) {
        type = 1;
    } else if (!strcasecmp((char *)attr[3], "out")) {
        type = 0;
    } else {
        ALOGE("%s: type must be IN or OUT", __func__);
        goto done;
    }

    if (strcmp(attr[4], "id") != 0) {
        ALOGE("%s: usecase id not mentioned", __func__);
        goto done;
    }

    int id = atoi((char *)attr[5]);

    if (platform_set_usecase_pcm_id(index, type, id) < 0) {
        ALOGE("%s: usecase %s type %d id %d was not set!",
              __func__, attr[1], type, id);
        goto done;
    }

done:
    return;
}

/* backend to be used for a device */
static void process_backend_name(const XML_Char **attr)
{
    int index;
    char *hw_interface = NULL;
    char *backend = NULL;

    if (strcmp(attr[0], "name") != 0) {
        ALOGE("%s: 'name' not found, no ACDB ID set!", __func__);
        goto done;
    }

    index = platform_get_snd_device_index((char *)attr[1]);
    if (index < 0) {
        ALOGE("%s: Device %s not found, no ACDB ID set!",
              __func__, attr[1]);
        goto done;
    }

    if (strcmp(attr[2], "backend") != 0) {
        if (strcmp(attr[2], "interface") == 0)
            hw_interface = (char *)attr[3];
    } else {
        backend = (char *)attr[3];
    }

    if (attr[4] != NULL) {
        if (strcmp(attr[4], "interface") != 0) {
            hw_interface = NULL;
        } else {
            hw_interface = (char *)attr[5];
        }
    }

    if (platform_set_snd_device_backend(index, backend, hw_interface) < 0) {
        ALOGE("%s: Device %s backend %s was not set!",
              __func__, attr[1], attr[3]);
        goto done;
    }

done:
    return;
}

static void process_gain_db_to_level_map(const XML_Char **attr)
{
    struct amp_db_and_gain_table tbl_entry;

    if ((strcmp(attr[0], "db") != 0) ||
        (strcmp(attr[2], "level") != 0)) {
        ALOGE("%s: invalid attribute passed  %s %sexpected amp db level",
               __func__, attr[0], attr[2]);
        goto done;
    }

    tbl_entry.db = atof(attr[1]);
    tbl_entry.amp = exp(tbl_entry.db * 0.115129f);
    tbl_entry.level = atoi(attr[3]);

    ALOGV("%s: amp [%f]  db [%f] level [%d]", __func__,
           tbl_entry.amp, tbl_entry.db, tbl_entry.level);
    platform_add_gain_level_mapping(&tbl_entry);

done:
    return;
}


static void process_acdb_id(const XML_Char **attr)
{
    int index;

    if (strcmp(attr[0], "name") != 0) {
        ALOGE("%s: 'name' not found, no ACDB ID set!", __func__);
        goto done;
    }

    index = platform_get_snd_device_index((char *)attr[1]);
    if (index < 0) {
        ALOGE("%s: Device %s in platform info xml not found, no ACDB ID set!",
              __func__, attr[1]);
        goto done;
    }

    if (strcmp(attr[2], "acdb_id") != 0) {
        ALOGE("%s: Device %s in platform info xml has no acdb_id, no ACDB ID set!",
              __func__, attr[1]);
        goto done;
    }

    if (platform_set_snd_device_acdb_id(index, atoi((char *)attr[3])) < 0) {
        ALOGE("%s: Device %s, ACDB ID %d was not set!",
              __func__, attr[1], atoi((char *)attr[3]));
        goto done;
    }

done:
    return;
}

static void process_bit_width(const XML_Char **attr)
{
    int index;

    if (strcmp(attr[0], "name") != 0) {
        ALOGE("%s: 'name' not found, no ACDB ID set!", __func__);
        goto done;
    }

    index = platform_get_snd_device_index((char *)attr[1]);
    if (index < 0) {
        ALOGE("%s: Device %s in platform info xml not found, no ACDB ID set!",
              __func__, attr[1]);
        goto done;
    }

    if (strcmp(attr[2], "bit_width") != 0) {
        ALOGE("%s: Device %s in platform info xml has no bit_width, no ACDB ID set!",
              __func__, attr[1]);
        goto done;
    }

    if (platform_set_snd_device_bit_width(index, atoi((char *)attr[3])) < 0) {
        ALOGE("%s: Device %s, ACDB ID %d was not set!",
              __func__, attr[1], atoi((char *)attr[3]));
        goto done;
    }

done:
    return;
}

static void process_interface_name(const XML_Char **attr)
{
    int ret;

    if (strcmp(attr[0], "name") != 0) {
        ALOGE("%s: 'name' not found, no Audio Interface set!", __func__);

        goto done;
    }

    if (strcmp(attr[2], "interface") != 0) {
        ALOGE("%s: Device %s has no Audio Interface set!",
              __func__, attr[1]);

        goto done;
    }

    if (strcmp(attr[4], "codec_type") != 0) {
        ALOGE("%s: Device %s has no codec type set!",
              __func__, attr[1]);

        goto done;
    }

    ret = platform_set_audio_device_interface((char *)attr[1], (char *)attr[3],
                                              (char *)attr[5]);
    if (ret < 0) {
        ALOGE("%s: Audio Interface not set!", __func__);
        goto done;
    }

done:
    return;
}

static void process_config_params(const XML_Char **attr)
{
    if (strcmp(attr[0], "key") != 0) {
        ALOGE("%s: 'key' not found", __func__);
        goto done;
    }

    if (strcmp(attr[2], "value") != 0) {
        ALOGE("%s: 'value' not found", __func__);
        goto done;
    }

    str_parms_add_str(my_data.kvpairs, (char*)attr[1], (char*)attr[3]);
done:
    return;
}

/* process acdb meta info key value */
static void process_acdb_metainfo_key(const XML_Char **attr)
{
    if (strcmp(attr[0], "name") != 0) {
        ALOGE("%s: 'name' not found", __func__);
        goto done;
    }

    if (strcmp(attr[2], "value") != 0) {
        ALOGE("%s: 'value' not found", __func__);
        goto done;
    }

    int key = atoi((char *)attr[3]);
    switch(my_data.caller) {
        case ACDB_EXTN:
                if(acdb_set_metainfo_key(my_data.platform, (char*)attr[1], key) < 0) {
                    ALOGE("%s: key %d was not set!", __func__, key);
                    goto done;
                }
                break;
        case PLATFORM:
                if(platform_set_acdb_metainfo_key(my_data.platform, (char*)attr[1], key) < 0) {
                    ALOGE("%s: key %d was not set!", __func__, key);
                    goto done;
                }
                break;
        default:
                ALOGE("%s: unknown caller!", __func__);
    }

done:
    return;
}

static void process_device_name(const XML_Char **attr)
{
    int index;

    if (strcmp(attr[0], "name") != 0) {
        ALOGE("%s: 'name' not found, no alias set!", __func__);
        goto done;
    }

    index = platform_get_snd_device_index((char *)attr[1]);
    if (index < 0) {
        ALOGE("%s: Device %s in platform info xml not found, no alias set!",
              __func__, attr[1]);
        goto done;
    }

    if (strcmp(attr[2], "alias") != 0) {
        ALOGE("%s: Device %s in platform info xml has no alias, no alias set!",
              __func__, attr[1]);
        goto done;
    }

    if (platform_set_snd_device_name(index, attr[3]) < 0) {
        ALOGE("%s: Device %s, alias %s was not set!",
              __func__, attr[1], attr[3]);
        goto done;
    }

done:
    return;
}

static void start_tag(void *userdata __unused, const XML_Char *tag_name,
                      const XML_Char **attr)
{
    if (my_data.caller == ACDB_EXTN) {
        if(strcmp(tag_name, "acdb_metainfo_key") == 0) {
            section = ACDB_METAINFO_KEY;
        } else if (strcmp(tag_name, "param") == 0) {
            if ((section != CONFIG_PARAMS) && (section != ACDB_METAINFO_KEY)) {
                ALOGE("param tag only supported with CONFIG_PARAMS section");
                return;
            }

            section_process_fn fn = section_table[section];
            fn(attr);
        }
    } else if(my_data.caller == PLATFORM) {
        if (strcmp(tag_name, "bit_width_configs") == 0) {
            section = BITWIDTH;
        } else if (strcmp(tag_name, "acdb_ids") == 0) {
            section = ACDB;
        } else if (strcmp(tag_name, "pcm_ids") == 0) {
            section = PCM_ID;
        } else if (strcmp(tag_name, "backend_names") == 0) {
            section = BACKEND_NAME;
        } else if (strcmp(tag_name, "config_params") == 0) {
            section = CONFIG_PARAMS;
        } else if (strcmp(tag_name, "interface_names") == 0) {
            section = INTERFACE_NAME;
        } else if (strcmp(tag_name, "gain_db_to_level_mapping") == 0) {
            section = GAIN_LEVEL_MAPPING;
        } else if(strcmp(tag_name, "acdb_metainfo_key") == 0) {
            section = ACDB_METAINFO_KEY;
        } else if (strcmp(tag_name, "device_names") == 0) {
            section = DEVICE_NAME;
        } else if (strcmp(tag_name, "device") == 0) {
            if ((section != ACDB) && (section != BACKEND_NAME) && (section != BITWIDTH) &&
                (section != INTERFACE_NAME) && (section != DEVICE_NAME)) {
                ALOGE("device tag only supported for acdb/backend names/bitwidth/interface/device names");
                return;
            }

            /* call into process function for the current section */
            section_process_fn fn = section_table[section];
            fn(attr);
        } else if (strcmp(tag_name, "gain_level_map") == 0) {
            if (section != GAIN_LEVEL_MAPPING) {
                ALOGE("usecase tag only supported with GAIN_LEVEL_MAPPING section");
                return;
            }

            section_process_fn fn = section_table[GAIN_LEVEL_MAPPING];
            fn(attr);
        } else if (strcmp(tag_name, "usecase") == 0) {
            if (section != PCM_ID) {
                ALOGE("usecase tag only supported with PCM_ID section");
                return;
            }

            section_process_fn fn = section_table[PCM_ID];
            fn(attr);
        } else if (strcmp(tag_name, "param") == 0) {
            if ((section != CONFIG_PARAMS) && (section != ACDB_METAINFO_KEY)) {
                ALOGE("param tag only supported with CONFIG_PARAMS section");
                return;
            }

            section_process_fn fn = section_table[section];
            fn(attr);
        }
    } else {
            ALOGE("%s: unknown caller!", __func__);
    }
    return;
}

static void end_tag(void *userdata __unused, const XML_Char *tag_name)
{
    if (strcmp(tag_name, "bit_width_configs") == 0) {
        section = ROOT;
    } else if (strcmp(tag_name, "acdb_ids") == 0) {
        section = ROOT;
    } else if (strcmp(tag_name, "pcm_ids") == 0) {
        section = ROOT;
    } else if (strcmp(tag_name, "backend_names") == 0) {
        section = ROOT;
    } else if (strcmp(tag_name, "config_params") == 0) {
        section = ROOT;
        if (my_data.caller == PLATFORM) {
            platform_set_parameters(my_data.platform, my_data.kvpairs);
        }
    } else if (strcmp(tag_name, "interface_names") == 0) {
        section = ROOT;
    } else if (strcmp(tag_name, "gain_db_to_level_mapping") == 0) {
        section = ROOT;
    } else if (strcmp(tag_name, "acdb_metainfo_key") == 0) {
        section = ROOT;
    } else if (strcmp(tag_name, "device_names") == 0) {
        section = ROOT;
    }
}

int platform_info_init(const char *filename, void *platform, caller_t caller_type)
{
    XML_Parser      parser;
    FILE            *file;
    int             ret = 0;
    int             bytes_read;
    void            *buf;

    file = fopen(filename, "r");
    section = ROOT;

    if (!file) {
        ALOGD("%s: Failed to open %s, using defaults.",
            __func__, filename);
        ret = -ENODEV;
        goto done;
    }

    parser = XML_ParserCreate(NULL);
    if (!parser) {
        ALOGE("%s: Failed to create XML parser!", __func__);
        ret = -ENODEV;
        goto err_close_file;
    }

    my_data.caller = caller_type;
    my_data.platform = platform;
    my_data.kvpairs = str_parms_create();

    XML_SetElementHandler(parser, start_tag, end_tag);

    while (1) {
        buf = XML_GetBuffer(parser, BUF_SIZE);
        if (buf == NULL) {
            ALOGE("%s: XML_GetBuffer failed", __func__);
            ret = -ENOMEM;
            goto err_free_parser;
        }

        bytes_read = fread(buf, 1, BUF_SIZE, file);
        if (bytes_read < 0) {
            ALOGE("%s: fread failed, bytes read = %d", __func__, bytes_read);
             ret = bytes_read;
            goto err_free_parser;
        }

        if (XML_ParseBuffer(parser, bytes_read,
                            bytes_read == 0) == XML_STATUS_ERROR) {
            ALOGE("%s: XML_ParseBuffer failed, for %s",
                __func__, filename);
            ret = -EINVAL;
            goto err_free_parser;
        }

        if (bytes_read == 0)
            break;
    }

err_free_parser:
    XML_ParserFree(parser);
err_close_file:
    fclose(file);
done:
    return ret;
}
