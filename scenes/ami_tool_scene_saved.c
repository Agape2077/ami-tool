#include "ami_tool_i.h"
#include <stdio.h>
#include <string.h>

#define SAVED_NAV_PREV (0xFFFFFFF0U)
#define SAVED_NAV_NEXT (0xFFFFFFF1U)
#define SAVED_NAV_INDEX (0xFFFFFFF2U)
#define SAVED_NAV_RESCAN (0xFFFFFFF3U)
#define SAVED_NAV_INDEX_ALL (0xFFFFFFF4U)
#define SAVED_NAV_BY_SERIES (0xFFFFFFF5U)
#define SAVED_NAV_BACK_ALL (0xFFFFFFF6U)
#define SAVED_NAV_BACK_SERIES (0xFFFFFFF7U)
#define SAVED_NAV_FAVORITES (0xFFFFFFF8U)
#define SAVED_NAV_SEARCH (0xFFFFFFF9U)
#define SAVED_NAV_UNRECOGNIZED (0xFFFFFFFAU)
#define SAVED_NAV_SERIES_ITEM (0xFFFF0000U)
#define SAVED_NAV_FILE_ITEM (0xFFFF1000U)

typedef enum {
    LibraryModeAll,
    LibraryModeSeriesList,
    LibraryModeSeriesItems,
    LibraryModeFavorites,
    LibraryModeSearch,
    LibraryModeUnrecognized,
} LibraryMode;

static LibraryMode library_mode = LibraryModeAll;
static size_t saved_page = 0;
static bool saved_detail_visible = false;
static bool saved_indexing_ui = false;

static AmiToolSeriesEntry series_entries[AMI_TOOL_SERIES_MAX];
static size_t series_count = 0;
static size_t series_selected = 0;
static size_t series_page = 0;
static size_t series_total = 0;
static size_t fav_page = 0;
static char search_query[24] = "";
static size_t search_page_index = 0;
static size_t unrec_page = 0;
static bool search_input_active = false;

static void ami_tool_scene_saved_build_menu(AmiToolApp* app);

static void ami_tool_scene_saved_show_text(AmiToolApp* app, const char* text) {
    furi_string_set(app->text_box_store, text);
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->text_box_store));
    view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewTextBox);
}

static void ami_tool_scene_saved_show_progress(AmiToolApp* app) {
    size_t total = 0;
    size_t done = ami_tool_library_index_progress(&total);
    char text[128];
    snprintf(
        text,
        sizeof(text),
        "Building library index...\n\n%u / %u files\n\nBack cancels.",
        (unsigned)done,
        (unsigned)total);
    furi_string_set(app->text_box_store, text);
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->text_box_store));
}

static void ami_tool_scene_saved_open_item(AmiToolApp* app, size_t local_index) {
    const AmiToolLibraryItem* item = ami_tool_library_page_item(local_index);
    if(!item) return;

    saved_detail_visible = true;

    if(!ami_tool_library_open(app, local_index)) {
        char text[224];
        snprintf(
            text,
            sizeof(text),
            "Cannot open this file as an Amiibo dump.\n\n%s\n\n"
            "Only MIFARE Ultralight / NTAG215 dumps are supported.",
            item->rel);
        ami_tool_scene_saved_show_text(app, text);
        return;
    }

    ami_tool_library_append_ids(app, saved_page);

    item = ami_tool_library_page_item(local_index);
    if(item->id[0] != '\0') {
        ami_tool_info_show_page(app, item->id, false);
    } else {
        char text[192];
        snprintf(
            text, sizeof(text), "Dump opened, but no Amiibo ID was found.\n\n%s", item->rel);
        ami_tool_scene_saved_show_text(app, text);
    }
}

static void ami_tool_scene_saved_search_callback(void* context) {
    AmiToolApp* app = context;
    app->text_input_active = false;
    search_input_active = false;
    search_page_index = 0;
    /* An empty query would match the whole library; just go back to it. */
    library_mode = (search_query[0] != '\0') ? LibraryModeSearch : LibraryModeAll;
    ami_tool_scene_saved_build_menu(app);
}

static void ami_tool_scene_saved_item_callback(void* context, uint32_t index) {
    AmiToolApp* app = context;

    if(index == SAVED_NAV_PREV) {
        if(library_mode == LibraryModeSeriesItems) {
            if(series_page > 0) series_page--;
        } else if(library_mode == LibraryModeFavorites) {
            if(fav_page > 0) fav_page--;
        } else if(library_mode == LibraryModeSearch) {
            if(search_page_index > 0) search_page_index--;
        } else if(library_mode == LibraryModeUnrecognized) {
            if(unrec_page > 0) unrec_page--;
        } else if(saved_page > 0) {
            saved_page--;
        }
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_NEXT) {
        if(library_mode == LibraryModeSeriesItems) {
            series_page++;
        } else if(library_mode == LibraryModeFavorites) {
            fav_page++;
        } else if(library_mode == LibraryModeSearch) {
            search_page_index++;
        } else if(library_mode == LibraryModeUnrecognized) {
            unrec_page++;
        } else {
            saved_page++;
        }
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_INDEX) {
        ami_tool_library_index_page(app, saved_page);
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_INDEX_ALL) {
        if(ami_tool_library_start_index_all(app)) {
            saved_indexing_ui = true;
            ami_tool_scene_saved_show_progress(app);
            view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewTextBox);
        } else {
            ami_tool_scene_saved_show_text(app, "Cannot start indexing.\nLibrary is empty.");
        }
        return;
    }
    if(index == SAVED_NAV_RESCAN) {
        ami_tool_library_rebuild(app);
        saved_page = 0;
        library_mode = LibraryModeAll;
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_BY_SERIES) {
        series_count = ami_tool_library_series_list(app, series_entries, AMI_TOOL_SERIES_MAX);
        library_mode = LibraryModeSeriesList;
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_FAVORITES) {
        fav_page = 0;
        library_mode = LibraryModeFavorites;
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_UNRECOGNIZED) {
        unrec_page = 0;
        library_mode = LibraryModeUnrecognized;
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_SEARCH) {
        search_input_active = true;
        app->text_input_active = true;
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Search name or series");
        text_input_set_result_callback(
            app->text_input,
            ami_tool_scene_saved_search_callback,
            app,
            search_query,
            sizeof(search_query),
            false);
        view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewTextInput);
        return;
    }
    if(index == SAVED_NAV_BACK_ALL) {
        library_mode = LibraryModeAll;
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if(index == SAVED_NAV_BACK_SERIES) {
        library_mode = LibraryModeSeriesList;
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if((index & 0xFFFFF000U) == SAVED_NAV_SERIES_ITEM) {
        size_t selected = index & 0x00000FFFU;
        if(selected >= series_count) return;
        series_selected = selected;
        series_page = 0;
        series_total =
            ami_tool_library_series_page(app, series_entries[selected].name, 0);
        library_mode = LibraryModeSeriesItems;
        ami_tool_scene_saved_build_menu(app);
        return;
    }
    if((index & 0xFFFFF000U) == SAVED_NAV_FILE_ITEM) {
        ami_tool_scene_saved_open_item(app, index & 0x00000FFFU);
        return;
    }

    /* LibraryModeAll: plain global index */
    size_t base = saved_page * AMI_TOOL_LIBRARY_PAGE;
    if(index < base) return;
    ami_tool_scene_saved_open_item(app, index - base);
}

static void ami_tool_scene_saved_add_paging(AmiToolApp* app, size_t page, size_t pages) {
    if(page > 0) {
        submenu_add_item(
            app->submenu, "< Previous page", SAVED_NAV_PREV, ami_tool_scene_saved_item_callback, app);
    }
    if(page + 1 < pages) {
        submenu_add_item(
            app->submenu, "Next page >", SAVED_NAV_NEXT, ami_tool_scene_saved_item_callback, app);
    }
}

static void ami_tool_scene_saved_build_menu(AmiToolApp* app) {
    submenu_reset(app->submenu);

    if(library_mode == LibraryModeSeriesList) {
        char header[48];
        snprintf(header, sizeof(header), "Series (%u)", (unsigned)series_count);
        submenu_set_header(app->submenu, header);

        submenu_add_item(
            app->submenu, "Back to library", SAVED_NAV_BACK_ALL, ami_tool_scene_saved_item_callback, app);

        for(size_t i = 0; i < series_count; i++) {
            char label[AMI_TOOL_LIBRARY_SERIES_MAX + 12];
            snprintf(
                label,
                sizeof(label),
                "%.20s (%u)",
                series_entries[i].name,
                (unsigned)series_entries[i].count);
            submenu_add_item(
                app->submenu,
                label,
                SAVED_NAV_SERIES_ITEM | (uint32_t)i,
                ami_tool_scene_saved_item_callback,
                app);
        }

        view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewMenu);
        return;
    }

    if(library_mode == LibraryModeSeriesItems) {
        size_t pages = (series_total + AMI_TOOL_LIBRARY_PAGE - 1) / AMI_TOOL_LIBRARY_PAGE;
        if(pages == 0) pages = 1;
        if(series_page >= pages) series_page = pages - 1;

        series_total = ami_tool_library_series_page(
            app, series_entries[series_selected].name, series_page);

        char header[64];
        snprintf(
            header,
            sizeof(header),
            "%.18s %u/%u",
            series_entries[series_selected].name,
            (unsigned)(series_page + 1),
            (unsigned)pages);
        submenu_set_header(app->submenu, header);

        submenu_add_item(
            app->submenu, "Back to series", SAVED_NAV_BACK_SERIES, ami_tool_scene_saved_item_callback, app);

        size_t count = ami_tool_library_page_count();
        for(size_t i = 0; i < count; i++) {
            const AmiToolLibraryItem* item = ami_tool_library_page_item(i);
            char label[AMI_TOOL_LIBRARY_NAME_MAX + 8];
            if(item->recognized) {
                snprintf(label, sizeof(label), "%s", item->name);
            } else {
                snprintf(label, sizeof(label), "[NFC] %.32s", item->rel);
            }
            submenu_add_item(
                app->submenu,
                label,
                SAVED_NAV_FILE_ITEM | (uint32_t)i,
                ami_tool_scene_saved_item_callback,
                app);
        }

        ami_tool_scene_saved_add_paging(app, series_page, pages);

        view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewMenu);
        return;
    }

    if(library_mode == LibraryModeFavorites) {
        size_t total = ami_tool_library_favorites_page(app, fav_page);
        size_t pages = (total + AMI_TOOL_LIBRARY_PAGE - 1) / AMI_TOOL_LIBRARY_PAGE;
        if(pages == 0) pages = 1;
        if(fav_page >= pages) {
            fav_page = pages - 1;
            total = ami_tool_library_favorites_page(app, fav_page);
        }

        char header[64];
        snprintf(
            header,
            sizeof(header),
            "Favorites %u/%u - %u",
            (unsigned)(fav_page + 1),
            (unsigned)pages,
            (unsigned)total);
        submenu_set_header(app->submenu, header);

        submenu_add_item(
            app->submenu, "Back to library", SAVED_NAV_BACK_ALL, ami_tool_scene_saved_item_callback, app);

        size_t count = ami_tool_library_page_count();
        for(size_t i = 0; i < count; i++) {
            const AmiToolLibraryItem* item = ami_tool_library_page_item(i);
            char label[AMI_TOOL_LIBRARY_NAME_MAX + 8];
            if(item->recognized) {
                snprintf(label, sizeof(label), "%s", item->name);
            } else {
                snprintf(label, sizeof(label), "[NFC] %.32s", item->rel);
            }
            submenu_add_item(
                app->submenu,
                label,
                SAVED_NAV_FILE_ITEM | (uint32_t)i,
                ami_tool_scene_saved_item_callback,
                app);
        }

        ami_tool_scene_saved_add_paging(app, fav_page, pages);

        view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewMenu);
        return;
    }

    if(library_mode == LibraryModeSearch) {
        size_t total = ami_tool_library_search_page(app, search_query, search_page_index);
        size_t pages = (total + AMI_TOOL_LIBRARY_PAGE - 1) / AMI_TOOL_LIBRARY_PAGE;
        if(pages == 0) pages = 1;
        if(search_page_index >= pages) {
            search_page_index = pages - 1;
            total = ami_tool_library_search_page(app, search_query, search_page_index);
        }

        char header[64];
        snprintf(
            header,
            sizeof(header),
            "\"%.12s\" %u/%u - %u",
            search_query,
            (unsigned)(search_page_index + 1),
            (unsigned)pages,
            (unsigned)total);
        submenu_set_header(app->submenu, header);

        submenu_add_item(
            app->submenu, "Back to library", SAVED_NAV_BACK_ALL, ami_tool_scene_saved_item_callback, app);

        size_t count = ami_tool_library_page_count();
        for(size_t i = 0; i < count; i++) {
            const AmiToolLibraryItem* item = ami_tool_library_page_item(i);
            char label[AMI_TOOL_LIBRARY_NAME_MAX + 8];
            if(item->recognized) {
                snprintf(label, sizeof(label), "%s", item->name);
            } else {
                snprintf(label, sizeof(label), "[NFC] %.32s", item->rel);
            }
            submenu_add_item(
                app->submenu,
                label,
                SAVED_NAV_FILE_ITEM | (uint32_t)i,
                ami_tool_scene_saved_item_callback,
                app);
        }

        ami_tool_scene_saved_add_paging(app, search_page_index, pages);

        view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewMenu);
        return;
    }

    if(library_mode == LibraryModeUnrecognized) {
        size_t total = ami_tool_library_unrecognized_page(app, unrec_page);
        size_t pages = (total + AMI_TOOL_LIBRARY_PAGE - 1) / AMI_TOOL_LIBRARY_PAGE;
        if(pages == 0) pages = 1;
        if(unrec_page >= pages) {
            unrec_page = pages - 1;
            total = ami_tool_library_unrecognized_page(app, unrec_page);
        }

        char header[64];
        snprintf(
            header,
            sizeof(header),
            "Unrecognized %u/%u - %u",
            (unsigned)(unrec_page + 1),
            (unsigned)pages,
            (unsigned)total);
        submenu_set_header(app->submenu, header);

        submenu_add_item(
            app->submenu, "Back to library", SAVED_NAV_BACK_ALL, ami_tool_scene_saved_item_callback, app);

        size_t count = ami_tool_library_page_count();
        for(size_t i = 0; i < count; i++) {
            const AmiToolLibraryItem* item = ami_tool_library_page_item(i);
            char label[AMI_TOOL_LIBRARY_NAME_MAX + 8];
            snprintf(label, sizeof(label), "[NFC] %.32s", item->rel);
            submenu_add_item(
                app->submenu,
                label,
                SAVED_NAV_FILE_ITEM | (uint32_t)i,
                ami_tool_scene_saved_item_callback,
                app);
        }

        ami_tool_scene_saved_add_paging(app, unrec_page, pages);

        view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewMenu);
        return;
    }

    size_t total = ami_tool_library_total();
    size_t pages = (total + AMI_TOOL_LIBRARY_PAGE - 1) / AMI_TOOL_LIBRARY_PAGE;
    if(pages == 0) pages = 1;
    if(saved_page >= pages) saved_page = pages - 1;

    ami_tool_library_load_page(app, saved_page);
    size_t count = ami_tool_library_page_count();

    char header[64];
    snprintf(
        header,
        sizeof(header),
        "Library %u/%u - %u files",
        (unsigned)(saved_page + 1),
        (unsigned)pages,
        (unsigned)total);
    submenu_set_header(app->submenu, header);

    size_t unknown = ami_tool_library_index_page_unknown();
    char index_label[48];
    if(unknown > 0) {
        snprintf(index_label, sizeof(index_label), "Index this page (%u new)", (unsigned)unknown);
    } else {
        snprintf(index_label, sizeof(index_label), "Index this page");
    }
    submenu_add_item(
        app->submenu, index_label, SAVED_NAV_INDEX, ami_tool_scene_saved_item_callback, app);
    submenu_add_item(
        app->submenu, "Index ALL files", SAVED_NAV_INDEX_ALL, ami_tool_scene_saved_item_callback, app);
    submenu_add_item(
        app->submenu, "Rescan library", SAVED_NAV_RESCAN, ami_tool_scene_saved_item_callback, app);
    submenu_add_item(
        app->submenu, "By series >", SAVED_NAV_BY_SERIES, ami_tool_scene_saved_item_callback, app);
    submenu_add_item(
        app->submenu, "Favorites", SAVED_NAV_FAVORITES, ami_tool_scene_saved_item_callback, app);
    submenu_add_item(
        app->submenu, "Search >", SAVED_NAV_SEARCH, ami_tool_scene_saved_item_callback, app);
    submenu_add_item(
        app->submenu, "Unrecognized", SAVED_NAV_UNRECOGNIZED, ami_tool_scene_saved_item_callback, app);

    size_t base = saved_page * AMI_TOOL_LIBRARY_PAGE;
    for(size_t i = 0; i < count; i++) {
        const AmiToolLibraryItem* item = ami_tool_library_page_item(i);
        char label[AMI_TOOL_LIBRARY_NAME_MAX + 8];
        if(item->recognized) {
            snprintf(label, sizeof(label), "%s", item->name);
        } else {
            snprintf(label, sizeof(label), "[NFC] %.32s", item->rel);
        }
        submenu_add_item(
            app->submenu, label, (uint32_t)(base + i), ami_tool_scene_saved_item_callback, app);
    }

    ami_tool_scene_saved_add_paging(app, saved_page, pages);

    view_dispatcher_switch_to_view(app->view_dispatcher, AmiToolViewMenu);
}

void ami_tool_scene_saved_on_enter(void* context) {
    AmiToolApp* app = context;
    furi_assert(app);

    saved_page = 0;
    library_mode = LibraryModeAll;
    saved_detail_visible = false;

    if(!app->storage) {
        ami_tool_scene_saved_show_text(app, "Storage service is unavailable.");
        return;
    }

    if(!ami_tool_library_ensure_index(app) || ami_tool_library_total() == 0) {
        ami_tool_scene_saved_show_text(
            app,
            "No .nfc files found in /ext/nfc/amiibo or its subfolders.\n\n"
            "Put Amiibo dumps there, then use Rescan library.");
        return;
    }

    ami_tool_scene_saved_build_menu(app);
}

bool ami_tool_scene_saved_on_event(void* context, SceneManagerEvent event) {
    AmiToolApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        if(!saved_indexing_ui) return false;

        if(ami_tool_library_index_finished()) {
            ami_tool_library_clear_finished();
            ami_tool_library_stop_index_all();
            saved_indexing_ui = false;
            ami_tool_scene_saved_build_menu(app);
        } else {
            ami_tool_scene_saved_show_progress(app);
        }
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case AmiToolEventInfoWriteStarted:
        case AmiToolEventInfoWriteSuccess:
        case AmiToolEventInfoWriteFailed:
        case AmiToolEventInfoWriteCancelled:
            ami_tool_info_handle_write_event(app, event.event);
            return true;
        case AmiToolEventInfoShowActions:
            ami_tool_info_show_actions_menu(app);
            return true;
        case AmiToolEventInfoActionEmulate:
            if(!ami_tool_info_start_emulation(app)) {
                ami_tool_info_show_action_message(
                    app,
                    "Unable to start emulation.\n\nOpen an Amiibo from the library first.\n"
                    "If one is already open, its UID could not be read,\nso the tag password cannot be derived.");
            }
            return true;
        case AmiToolEventInfoActionUsage:
            ami_tool_info_show_usage(app);
            return true;
        case AmiToolEventInfoActionChangeUid:
            if(!ami_tool_info_change_uid(app)) {
                ami_tool_info_show_action_message(
                    app, "Unable to change UID.\nInstall key_retail.bin and try again.");
            }
            return true;
        case AmiToolEventInfoActionWriteTag:
            if(!ami_tool_info_write_to_tag(app)) {
                ami_tool_info_show_action_message(
                    app,
                    "Unable to write tag.\nUse a blank NTAG215 and make\nsure key_retail.bin is installed.");
            }
            return true;
        case AmiToolEventInfoActionSaveToStorage:
            if(!ami_tool_info_save_to_storage(app)) {
                ami_tool_info_show_action_message(
                    app, "Unable to save Amiibo.\nCheck storage access.");
            }
            return true;
        case AmiToolEventUsagePrevPage:
            ami_tool_info_navigate_usage(app, -1);
            return true;
        case AmiToolEventUsageNextPage:
            ami_tool_info_navigate_usage(app, 1);
            return true;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(app->text_input_active) {
            app->text_input_active = false;
            ami_tool_info_show_actions_menu(app);
            return true;
        }
        if(search_input_active) {
            search_input_active = false;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
        if(saved_indexing_ui) {
            ami_tool_library_stop_index_all();
            ami_tool_library_clear_finished();
            saved_indexing_ui = false;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
        if(app->write_in_progress) {
            if(app->write_waiting_for_tag) {
                ami_tool_info_request_write_cancel(app);
            }
            return true;
        }
        if(app->info_emulation_active) {
            ami_tool_info_stop_emulation(app);
            ami_tool_info_show_actions_menu(app);
            return true;
        }
        if(app->usage_info_visible) {
            app->usage_info_visible = false;
            ami_tool_info_show_actions_menu(app);
            return true;
        }
        if(app->info_action_message_visible) {
            ami_tool_info_show_actions_menu(app);
            return true;
        }
        if(app->info_menu_depth >= 2) {
            ami_tool_info_show_actions_menu(app);
            return true;
        }
        if(app->info_actions_visible) {
            app->info_actions_visible = false;
            app->info_menu_depth = 0;
            ami_tool_info_refresh_current_page(app);
            return true;
        }
        if(saved_detail_visible) {
            saved_detail_visible = false;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
        if(library_mode == LibraryModeSeriesItems) {
            library_mode = LibraryModeSeriesList;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
        if(library_mode == LibraryModeFavorites) {
            library_mode = LibraryModeAll;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
        if(library_mode == LibraryModeSearch) {
            library_mode = LibraryModeAll;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
        if(library_mode == LibraryModeUnrecognized) {
            library_mode = LibraryModeAll;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
        if(library_mode == LibraryModeSeriesList) {
            library_mode = LibraryModeAll;
            ami_tool_scene_saved_build_menu(app);
            return true;
        }
    }

    return false;
}

void ami_tool_scene_saved_on_exit(void* context) {
    AmiToolApp* app = context;
    if(app) app->text_input_active = false;
    ami_tool_library_stop_index_all();
    ami_tool_library_clear_finished();
    saved_indexing_ui = false;
    search_input_active = false;
    saved_page = 0;
    series_page = 0;
    fav_page = 0;
    unrec_page = 0;
    library_mode = LibraryModeAll;
    saved_detail_visible = false;
}
