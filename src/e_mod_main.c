#include "e.h"
#include "config.h"
#include "e_mod_main.h"
#include "e_mod_config.h"
#include "e_border.h"
#include "e_shelf.h"

#include <math.h>
#include <stdbool.h>
#include <assert.h>

typedef enum {
    TILING_RESIZE,
    TILING_MOVE,
} tiling_change_t;

/* actual module specifics */

typedef struct Border_Extra {
     E_Border *border;
     int x,
         y,
         w,
         h;
} Border_Extra;

struct tiling_g tiling_g = {
   .module = NULL,
   .config = NULL,
   .log_domain = -1,
};

static struct
{
   E_Config_DD         *config_edd,
                       *vdesk_edd;
   E_Border_Hook       *hook;
   int                  currently_switching_desktop;
   Ecore_Event_Handler *handler_hide,
                       *handler_desk_show,
                       *handler_desk_before_show,
                       *handler_mouse_move,
                       *handler_desk_set;
   E_Zone              *current_zone;

   Tiling_Info         *tinfo;
   /* This hash holds the Tiling_Info-pointers for each desktop */
   Eina_Hash           *info_hash;

   Eina_Hash           *border_extras;

   E_Action            *act_toggletiling,
                       *act_togglefloat,
                       *act_switchtiling;
} tiling_mod_main_g = {
#define _G tiling_mod_main_g
   .hook = NULL,
   .currently_switching_desktop = 0,
   .handler_hide = NULL,
   .handler_desk_show = NULL,
   .handler_desk_before_show = NULL,
   .handler_mouse_move = NULL,
   .handler_desk_set = NULL,
   .current_zone = NULL,
   .tinfo = NULL,
   .info_hash = NULL,
   .border_extras = NULL,

   .act_toggletiling = NULL,
   .act_togglefloat = NULL,
   .act_switchtiling = NULL,
};


/* Utils {{{ */

#define EINA_LIST_IS_IN(_list, _el) \
    (eina_list_data_find(_list, _el) == _el)
#define EINA_LIST_APPEND(_list, _el) \
    _list = eina_list_append(_list, _el)
#define EINA_LIST_REMOVE(_list, _el) \
    _list = eina_list_remove(_list, _el)

/* I wonder why noone has implemented the following one yet? */
static E_Desk *
get_current_desk(void)
{
    E_Manager *m = e_manager_current_get();
    E_Container *c = e_container_current_get(m);
    E_Zone *z = e_zone_current_get(c);

    return e_desk_current_get(z);
}

static Tiling_Info *
_initialize_tinfo(const E_Desk *desk)
{
    Tiling_Info *tinfo;

    tinfo = E_NEW(Tiling_Info, 1);
    tinfo->desk = desk;
    tinfo->need_rearrange = 0;
    eina_hash_direct_add(_G.info_hash, &tinfo->desk, tinfo);

    tinfo->conf = get_vdesk(tiling_g.config->vdesks, desk->x, desk->y,
                          desk->zone->num);

    return tinfo;
}

static void
check_tinfo(const E_Desk *desk)
{
    if (!_G.tinfo || _G.tinfo->desk != desk) {
        _G.tinfo = eina_hash_find(_G.info_hash, &desk);
        if (!_G.tinfo) {
            /* lazy init */
            DBG("need new info for %s\n", desk->name);
            _G.tinfo = _initialize_tinfo(desk);
        }
        if (!_G.tinfo->conf) {
            _G.tinfo->conf = get_vdesk(tiling_g.config->vdesks,
                                       desk->x, desk->y,
                                       desk->zone->num);
        }
    }
}

static int
is_floating_window(const E_Border *bd)
{
    check_tinfo(bd->desk);
    return (eina_list_data_find(_G.tinfo->floating_windows, bd) == bd);
}

static int
is_untilable_dialog(const E_Border *bd)
{
    return (!tiling_g.config->tile_dialogs
    && ((bd->client.icccm.transient_for != 0)
         || (bd->client.netwm.type == ECORE_X_WINDOW_TYPE_DIALOG)));
}

static void
change_window_border(E_Border   *bd,
                     const char *bordername)
{
   eina_stringshare_replace(&bd->bordername, bordername);
   bd->client.border.changed = 1;
   bd->changed = 1;
}

static int
get_column(const E_Border *bd)
{
    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        if (EINA_LIST_IS_IN(_G.tinfo->columns[i], bd))
            return i;
    }
    return -1;
}

static int
get_column_count(void)
{
    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        if (!_G.tinfo->columns[i])
            return i;
    }
    return TILING_MAX_COLUMNS;
}
/* }}} */
/* Reorganize windows {{{*/

static void
_reorganize_column(int col)
{
    int zx, zy, zw, zh, x, w, h, ch, i = 0, count;

    if (col < 0 || col >= TILING_MAX_COLUMNS
        || !_G.tinfo->columns[col])
        return;

    e_zone_useful_geometry_get(_G.tinfo->desk->zone, &zx, &zy, &zw, &zh);
    DBG("useful geometry: %dx%d+%d+%d", zw, zh, zx, zy);

    count = eina_list_count(_G.tinfo->columns[col]);

    x = _G.tinfo->x[col];
    ch = 0;
    w = _G.tinfo->w[col];
    h = zh / count;

    for (Eina_List *l = _G.tinfo->columns[col]; l; l = l->next, i++) {
        E_Border *bd = l->data;
        Border_Extra *extra;
        int d = (i * 2 * zh) % count
              - (2 * ch) % count;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        if ((bd->maximized & E_MAXIMIZE_VERTICAL) && count != 1) {
            e_border_unmaximize(bd, E_MAXIMIZE_VERTICAL);
        }
        /* let's use a bresenham here */

        extra->x = x;
        extra->y = ch + zy;
        extra->w = w;
        extra->h = h + d;
        ch += extra->h;
        DBG("%p: d = %d, ch = %d, (%dx%d+%d+%d)", bd, d, ch,
            extra->w, extra->h, extra->x, extra->y);

        e_border_move_resize(bd, extra->x,
                                 extra->y,
                                 extra->w,
                                 extra->h);
    }
}

static void
_move_resize_column(Eina_List *list, int delta_x, int delta_w)
{
    /* TODO: is alone */
    for (Eina_List *l = list; l; l = l->next) {
        E_Border *bd = l->data;
        Border_Extra *extra;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        extra->x += delta_x;
        extra->w += delta_w;

        e_border_move_resize(bd, extra->x,
                                 extra->y,
                                 extra->w,
                                 extra->h);
    }
}

static void
_set_column_geometry(int col, int x, int w)
{
    for (Eina_List *l = _G.tinfo->columns[col]; l; l = l->next) {
        E_Border *bd = l->data;
        Border_Extra *extra;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        extra->x = x;
        extra->w = w;

        if (bd->maximized & E_MAXIMIZE_VERTICAL) {
            e_border_unmaximize(bd, E_MAXIMIZE_HORIZONTAL);
        }

        e_border_move_resize(bd, extra->x,
                                 extra->y,
                                 extra->w,
                                 extra->h);
    }
    _G.tinfo->x[col] = x;
    _G.tinfo->w[col] = w;
}


static void
_add_border(E_Border *bd)
{
    Border_Extra *extra;

    if (!bd) {
        return;
    }
    if (is_floating_window(bd)) {
        DBG("floating window");
        return;
    }
    if (is_untilable_dialog(bd)) {
        DBG("untilable_dialog");
        return;
    }

    if (!_G.tinfo->conf || !_G.tinfo->conf->nb_cols) {
        DBG("no tiling");
        return;
    }

    extra = E_NEW(Border_Extra, 1);
    *extra = (Border_Extra) {
        .border = bd,
            .x = bd->x,
            .y = bd->y,
            .w = bd->w,
            .h = bd->h
    };

    eina_hash_direct_add(_G.border_extras, &extra->border, extra);

    /* New Border! */
    DBG("new border");

    if ((bd->bordername && strcmp(bd->bordername, "pixel"))
    ||  !bd->bordername)
    {
        change_window_border(bd, "pixel");
    }

    if (_G.tinfo->columns[0]) {
        if (_G.tinfo->columns[_G.tinfo->conf->nb_cols - 1]) {
            int col = _G.tinfo->conf->nb_cols - 1;

            if (!_G.tinfo->columns[col]->next) {
                e_border_unmaximize(_G.tinfo->columns[col]->data,
                                    E_MAXIMIZE_BOTH);
            }
            EINA_LIST_APPEND(_G.tinfo->columns[col], bd);
            _reorganize_column(col);
        } else {
            /* Add column */
            int nb_cols = get_column_count();
            int x, y, w, h;
            int width = 0;

            e_zone_useful_geometry_get(bd->zone, &x, &y, &w, &h);

            for (int i = 0; i < nb_cols; i++) {

                width = w / (nb_cols + 1 - i);

                _set_column_geometry(i, x, width);

                w -= width;
                x += width;
            }

            _G.tinfo->x[nb_cols] = x;
            _G.tinfo->w[nb_cols] = width;
            extra->x = x;
            extra->y = y;
            extra->w = width;
            extra->h = h;
            e_border_move_resize(bd,
                                 extra->x,
                                 extra->y,
                                 extra->w,
                                 extra->h);
            e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_VERTICAL);

            EINA_LIST_APPEND(_G.tinfo->columns[nb_cols], bd);
        }
    } else {
        e_border_unmaximize(bd, E_MAXIMIZE_BOTH);
        e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
        EINA_LIST_APPEND(_G.tinfo->columns[0], bd);
        e_zone_useful_geometry_get(bd->zone,
                                   &_G.tinfo->x[0], NULL,
                                   &_G.tinfo->w[0], NULL);
    }
}

static void
_remove_border(E_Border *bd)
{
    int col;

    check_tinfo(bd->desk);

    col = get_column(bd);
    if (col < 0)
        return;

    EINA_LIST_REMOVE(_G.tinfo->columns[col], bd);
    eina_hash_del(_G.border_extras, bd, NULL);
    if (_G.tinfo->columns[col]) {
        _reorganize_column(col);
    } else {
        /* Remove column */
        int nb_cols = get_column_count();
        int x, y, w, h;
        int width = 0;

        e_zone_useful_geometry_get(bd->zone, &x, &y, &w, &h);

        for (int i = 0; i < nb_cols; i++) {

            width = w / (nb_cols - i);

            _set_column_geometry(i, x, width);

            w -= width;
            x += width;
        }
    }
}

static void
_move_resize_border_column(E_Border *bd, Border_Extra *extra,
                           int col, tiling_change_t change)
{
    if (change == TILING_RESIZE) {
        if (col == TILING_MAX_COLUMNS || !_G.tinfo->columns[col + 1]) {
            /* You're not allowed to resize */
            bd->w = extra->w;
        } else {
            int delta = bd->w - extra->w;

            _move_resize_column(_G.tinfo->columns[col], 0, delta);
            _move_resize_column(_G.tinfo->columns[col+1], delta, -delta);
            extra->w = bd->w;
        }
    } else {
        if (col == 0) {
            /* You're not allowed to move */
            bd->x = extra->x;
        } else {
            int delta = bd->x - extra->x;

            _move_resize_column(_G.tinfo->columns[col], delta, -delta);
            _move_resize_column(_G.tinfo->columns[col-1], 0, delta);
            extra->x = bd->x;
        }
    }
}

static void
_move_resize_border_in_column(E_Border *bd, Border_Extra *extra,
                              int col, tiling_change_t change)
{
    Eina_List *l;

    l = eina_list_data_find_list(_G.tinfo->columns[col], bd);
    if (!l)
        return;

    if (change == TILING_RESIZE) {
        if (!l->next) {
            /* You're not allowed to resize */
            bd->h = extra->h;
        } else {
            int delta = bd->h - extra->h;
            E_Border *nextbd = l->next->data;
            Border_Extra *nextextra;
            int min_height = MAX(nextbd->client.icccm.base_h, 1);

            nextextra = eina_hash_find(_G.border_extras, &nextbd);
            if (!nextextra) {
                ERR("No extra for %p", nextbd);
                return;
            }

            if (nextextra->h - delta < min_height)
                delta = nextextra->h - min_height;

            nextextra->y += delta;
            nextextra->h -= delta;
            e_border_move_resize(nextbd, nextextra->x, nextextra->y,
                                         nextextra->w, nextextra->h);

            extra->h += delta;
            bd->h = extra->h;
        }
    } else {
        if (!l->prev) {
            /* You're not allowed to move */
            bd->y = extra->y;
        } else {
            int delta = bd->y - extra->y;
            E_Border *prevbd = l->prev->data;
            Border_Extra *prevextra;
            int min_height = MAX(prevbd->client.icccm.base_h, 1);

            prevextra = eina_hash_find(_G.border_extras, &prevbd);
            if (!prevextra) {
                ERR("No extra for %p", prevbd);
                return;
            }

            if (prevextra->h - delta < min_height)
                delta = prevextra->h - min_height;

            prevextra->h += delta;
            e_border_move_resize(prevbd, prevextra->x, prevextra->y,
                                         prevextra->w, prevextra->h);

            extra->y += delta;
            extra->h -= delta;
            bd->y = extra->y;
            bd->h = extra->h;
        }
    }
}

/* }}} */
/* Toggle Floating {{{ */

static void
toggle_floating(E_Border *bd)
{
    if (!bd || !_G.tinfo)
        return;

    check_tinfo(bd->desk);

    if (eina_list_data_find(_G.tinfo->floating_windows, bd) == bd) {
        _G.tinfo->floating_windows =
            eina_list_remove(_G.tinfo->floating_windows, bd);

        _add_border(bd);
    } else {
        /* To give the user a bit of feedback we restore the original border */
        /* TODO: save the original border, don't just restore the default one*/
        _G.tinfo->floating_windows =
            eina_list_prepend(_G.tinfo->floating_windows, bd);

        _remove_border(bd);

        e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);

        change_window_border(bd, "default");
    }
}

/* }}} */
/* Action callbacks {{{*/

static void
_e_mod_action_toggle_floating_cb(E_Object   *obj,
                                 const char *params)
{
    toggle_floating(e_border_focused_get());
}

/* }}} */
/* Hooks {{{*/

static void
_e_module_tiling_cb_hook(void *data,
                         void *border)
{
    E_Border *bd = border;
    int col = -1;

    DBG("cb-Hook for %p", bd);
    if (!bd) {
        return;
    }
    if (is_floating_window(bd)) {
        DBG("floating window");
        return;
    }
    if (is_untilable_dialog(bd)) {
        DBG("untilable_dialog");
        return;
    }

    if (!_G.tinfo->conf || !_G.tinfo->conf->nb_cols) {
        DBG("no tiling");
        return;
    }

    col = get_column(bd);

    DBG("cb-Hook for %p / %s / %s, changes(size=%d, position=%d, border=%d)"
        " g:%dx%d+%d+%d bdname:%s (%d) %d",
        bd, bd->client.icccm.title, bd->client.netwm.name,
        bd->changes.size, bd->changes.pos, bd->changes.border,
        bd->w, bd->h, bd->x, bd->y, bd->bordername,
        col, bd->maximized);

    if (!bd->changes.size && !bd->changes.pos && !bd->changes.border
    && (col >= 0)) {
        DBG("nothing to do");
        return;
    }

    if (col < 0) {
        _add_border(bd);
    } else {
        Border_Extra *extra;

        /* Move or Resize */
        DBG("move or resize");

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            return;
        }

        if (col == 0 && !_G.tinfo->columns[1] && !_G.tinfo->columns[0]->next) {
            DBG("forever alone :)");
            if (bd->maximized) {
                extra->x = bd->x;
                extra->y = bd->y;
                extra->w = bd->w;
                extra->h = bd->h;
            } else {
                /* TODO: what if a window doesn't want to be maximized? */
                e_border_unmaximize(bd, E_MAXIMIZE_BOTH);
                e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
            }
        }
        if (bd->x == extra->x && bd->y == extra->y
        &&  bd->w == extra->w && bd->h == extra->h)
        {
            return;
        }

        if (bd->changes.border && bd->changes.size) {
            e_border_move_resize(bd, extra->x, extra->y,
                                     extra->w, extra->h);
            return;
        }

        DBG("old:%dx%d+%d+%d vs new:%dx%d+%d+%d. step:%dx%d. base:%dx%d",
            extra->w, extra->h, extra->x, extra->y,
            bd->w, bd->h, bd->x, bd->y,
            bd->client.icccm.step_w, bd->client.icccm.step_h,
            bd->client.icccm.base_w, bd->client.icccm.base_h);

        if (abs(extra->w - bd->w) >= bd->client.icccm.step_w) {
            _move_resize_border_column(bd, extra, col, TILING_RESIZE);
        }
        if (abs(extra->h - bd->h) >= bd->client.icccm.step_h) {
            _move_resize_border_in_column(bd, extra, col, TILING_RESIZE);
        }
        if (extra->x != bd->x) {
            _move_resize_border_column(bd, extra, col, TILING_MOVE);
        }
        if (extra->y != bd->y) {
            _move_resize_border_in_column(bd, extra, col, TILING_MOVE);
        }
    }
}

static Eina_Bool
_e_module_tiling_hide_hook(void *data,
                           int   type,
                           void *event)
{
    E_Event_Border_Hide *ev = event;
    E_Border *bd = ev->border;

    if (_G.currently_switching_desktop)
        return EINA_TRUE;

    DBG("hide-hook for %p", bd);

    check_tinfo(bd->desk);

    if (EINA_LIST_IS_IN(_G.tinfo->floating_windows, bd)) {
        EINA_LIST_REMOVE(_G.tinfo->floating_windows, bd);
    }

    _remove_border(bd);

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_show(void *data,
                           int   type,
                           void *event)
{
    _G.currently_switching_desktop = 0;

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_before_show(void *data,
                                  int   type,
                                  void *event)
{
    _G.currently_switching_desktop = 1;

    return EINA_TRUE;
}

static Eina_Bool
_clear_bd_from_info_hash(const Eina_Hash *hash,
                         const void      *key,
                         void            *data,
                         void            *fdata)
{
    Tiling_Info *ti = data;
    E_Event_Border_Desk_Set *ev = fdata;

    if (!ev || !ti)
        return EINA_TRUE;

    if (ti->desk == ev->desk) {
        ti->need_rearrange = 1;
        DBG("set need_rearrange=1\n");
        return EINA_TRUE;
    }

    /* TODO
    if (eina_list_data_find(ti->client_list, ev->border) == ev->border) {
        ti->client_list = eina_list_remove(ti->client_list, ev->border);
        if (ti->desk == get_current_desk()) {
            E_Border *first;

            if ((first = get_first_window(NULL, ti->desk)))
                rearrange_windows(first, EINA_FALSE);
        }
    }
    */

    if (eina_list_data_find(ti->floating_windows, ev->border) == ev->border) {
        ti->floating_windows = eina_list_remove(ti->floating_windows,
                                                ev->border);
    }

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_set(void *data,
                          int   type,
                          void *event)
{
    /* We use this event to ensure that border desk changes are done correctly
     * because a user can move the window to another desk (and events are
     * fired) involving zone changes or not (depends on the mouse position) */
    E_Event_Border_Desk_Set *ev = event;
    Tiling_Info *tinfo;

    tinfo = eina_hash_find(_G.info_hash, &ev->desk);

    if (!tinfo) {
        DBG("create new info for %s\n", ev->desk->name);
        tinfo = _initialize_tinfo(ev->desk);
    }

    eina_hash_foreach(_G.info_hash, _clear_bd_from_info_hash, ev);
    DBG("desk set\n");

    return EINA_TRUE;
}

/* }}} */
/* Module setup {{{*/

static Eina_Bool
_clear_info_hash(const Eina_Hash *hash,
                 const void      *key,
                 void            *data,
                 void            *fdata)
{
    Tiling_Info *ti = data;

    eina_list_free(ti->floating_windows);
    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        eina_list_free(ti->columns[i]);
        ti->columns[i] = NULL;
    }
    E_FREE(ti);

    return EINA_TRUE;
}

static Eina_Bool
_clear_border_extras(const Eina_Hash *hash,
                     const void      *key,
                     void            *data,
                     void            *fdata)
{
    Border_Extra *be = data;

    E_FREE(be);

    return EINA_TRUE;
}

EAPI E_Module_Api e_modapi =
{
    E_MODULE_API_VERSION,
    "Tiling"
};

EAPI void *
e_modapi_init(E_Module *m)
{
    char buf[PATH_MAX];
    E_Desk *desk;

    tiling_g.module = m;

    if (tiling_g.log_domain < 0) {
        tiling_g.log_domain = eina_log_domain_register("tiling", NULL);
        if (tiling_g.log_domain < 0) {
            EINA_LOG_CRIT("could not register log domain 'tiling'");
        }
    }


    snprintf(buf, sizeof(buf), "%s/locale", e_module_dir_get(m));
    bindtextdomain(PACKAGE, buf);
    bind_textdomain_codeset(PACKAGE, "UTF-8");

    _G.info_hash = eina_hash_pointer_new(NULL);

    _G.border_extras = eina_hash_pointer_new(NULL);

    /* Callback for new windows or changes */
    _G.hook = e_border_hook_add(E_BORDER_HOOK_EVAL_POST_BORDER_ASSIGN,
                                _e_module_tiling_cb_hook, NULL);
    /* Callback for hiding windows */
    _G.handler_hide = ecore_event_handler_add(E_EVENT_BORDER_HIDE,
                                             _e_module_tiling_hide_hook, NULL);
    /* Callback when virtual desktop changes */
    _G.handler_desk_show = ecore_event_handler_add(E_EVENT_DESK_SHOW,
                                             _e_module_tiling_desk_show, NULL);
    /* Callback before virtual desktop changes */
    _G.handler_desk_before_show =
        ecore_event_handler_add(E_EVENT_DESK_BEFORE_SHOW,
                                _e_module_tiling_desk_before_show, NULL);
    /* Callback when the mouse moves */
    /*
    _G.handler_mouse_move = ecore_event_handler_add(ECORE_EVENT_MOUSE_MOVE,
                                            _e_module_tiling_mouse_move, NULL);
    */
    /* Callback when a border is set to another desk */
    _G.handler_desk_set = ecore_event_handler_add(E_EVENT_BORDER_DESK_SET,
                                              _e_module_tiling_desk_set, NULL);

#define ACTION_ADD(_act, _cb, _title, _value)                                \
{                                                                            \
   E_Action *_action = _act;                                                 \
   const char *_name = _value;                                               \
   if ((_action = e_action_add(_name)))                                      \
     {                                                                       \
        _action->func.go = _cb;                                              \
        e_action_predef_name_set(D_("Tiling"), D_(_title), _name,            \
                                 NULL, NULL, 0);                             \
     }                                                                       \
}

    /* Module's actions */
    ACTION_ADD(_G.act_togglefloat, _e_mod_action_toggle_floating_cb,
               "Toggle floating", "toggle_floating");
#undef ACTION_ADD

    /* Configuration entries */
    snprintf(buf, sizeof(buf), "%s/e-module-tiling.edj", e_module_dir_get(m));
    e_configure_registry_category_add("windows", 50, D_("Windows"), NULL,
                                      "preferences-system-windows");
    e_configure_registry_item_add("windows/tiling", 150, D_("Tiling"), NULL,
                                  buf, e_int_config_tiling_module);

    /* Configuration itself */
    _G.config_edd = E_CONFIG_DD_NEW("Tiling_Config", Config);
    _G.vdesk_edd = E_CONFIG_DD_NEW("Tiling_Config_VDesk",
                                   struct _Config_vdesk);
    E_CONFIG_VAL(_G.config_edd, Config, tile_dialogs, INT);
    E_CONFIG_VAL(_G.config_edd, Config, float_too_big_windows, INT);

    E_CONFIG_LIST(_G.config_edd, Config, vdesks, _G.vdesk_edd);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, x, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, y, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, zone_num, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, nb_cols, INT);

    tiling_g.config = e_config_domain_load("module.tiling", _G.config_edd);
    if (!tiling_g.config) {
        tiling_g.config = E_NEW(Config, 1);
        tiling_g.config->float_too_big_windows = 1;
        tiling_g.config->tile_dialogs = 1;
    }

    E_CONFIG_LIMIT(tiling_g.config->tile_dialogs, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->float_too_big_windows, 0, 1);

    desk = get_current_desk();
    _G.current_zone = desk->zone;
    _G.tinfo = _initialize_tinfo(desk);

    DBG("initialized");
    return m;
}

EAPI int
e_modapi_shutdown(E_Module *m)
{

    if (tiling_g.log_domain >= 0) {
        DBG("shutdown!");
        eina_log_domain_unregister(tiling_g.log_domain);
        tiling_g.log_domain = -1;
    }

    if (_G.hook) {
        e_border_hook_del(_G.hook);
        _G.hook = NULL;
    }

#define FREE_HANDLER(x)          \
if (x) {                         \
     ecore_event_handler_del(x); \
     x = NULL;                   \
}
    FREE_HANDLER(_G.handler_hide);
    FREE_HANDLER(_G.handler_desk_show);
    FREE_HANDLER(_G.handler_desk_before_show);
    FREE_HANDLER(_G.handler_mouse_move);
    FREE_HANDLER(_G.handler_desk_set);
#undef FREE_HANDLER


#define ACTION_DEL(act, title, value)                   \
if (act) {                                              \
     e_action_predef_name_del(D_("Tiling"), D_(title)); \
     e_action_del(value);                               \
     act = NULL;                                        \
}
    ACTION_DEL(_G.act_toggletiling, "Toggle tiling", "toggle_tiling");
    ACTION_DEL(_G.act_togglefloat, "Toggle floating", "toggle_floating");
    ACTION_DEL(_G.act_switchtiling, "Switch tiling mode", "switch_tiling");
#undef ACTION_DEL

    e_configure_registry_item_del("windows/tiling");
    e_configure_registry_category_del("windows");

    E_FREE(tiling_g.config);
    E_CONFIG_DD_FREE(_G.config_edd);
    E_CONFIG_DD_FREE(_G.vdesk_edd);

    tiling_g.module = NULL;

    eina_hash_foreach(_G.info_hash, _clear_info_hash, NULL);
    eina_hash_free(_G.info_hash);
    _G.info_hash = NULL;

    eina_hash_foreach(_G.info_hash, _clear_border_extras, NULL);
    eina_hash_free(_G.border_extras);
    _G.border_extras = NULL;

    _G.tinfo = NULL;

    return 1;
}

EAPI int
e_modapi_save(E_Module *m)
{
    e_config_domain_save("module.tiling", _G.config_edd, tiling_g.config);
    /* TODO */
    DBG("SAVE");

    return EINA_TRUE;
}
/* }}} */
