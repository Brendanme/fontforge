/* Copyright (C) 2016 by Jeremy Tan */
/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.

 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <fontforge-config.h>

#ifdef FONTFORGE_CAN_USE_QT

#include "gqtdrawP.h"
#include "ustring.h"

#include <QtWidgets>
#include <type_traits>

namespace fontforge { namespace gdraw {

// Forward declarations
static void GQtDrawCancelTimer(GTimer *timer);
static void GQtDrawDestroyWindow(GWindow w);
static void GQtDrawPostEvent(GEvent *e);
static void GQtDrawProcessPendingEvents(GDisplay *disp);
static void GQtDrawSetCursor(GWindow w, GCursor gcursor);
static void GQtDrawSetTransientFor(GWindow transient, GWindow owner);
static void GQtDrawSetWindowBackground(GWindow w, Color gcol);


static GGC *_GQtDraw_NewGGC(void) {
    GGC *ggc = new GGC();
    ggc->clip.width = ggc->clip.height = 0x7fff;
    ggc->fg = 0;
    ggc->bg = 0xffffff;
    return ggc;
}

static int16_t _GQtDraw_QtModifierToKsm(Qt::KeyboardModifiers mask) {
    int16_t state = 0;
    if (mask & Qt::ShiftModifier) {
        state |= ksm_shift;
    }
    if (mask & Qt::ControlModifier) {
        state |= ksm_control;
    }
    if (mask & Qt::AltModifier) {
        state |= ksm_meta;
    }
    if (mask & Qt::MetaModifier) {
        state |= ksm_meta;
    }
    return state;
}

static int _GQtDraw_WindowOrParentsDying(GWindow w) {
    while (w != nullptr) {
        if (w->is_dying) {
            return true;
        }
        if (w->is_toplevel) {
            return false;
        }
        w = w->parent;
    }
    return false;
}

static void _GQtDraw_CallEHChecked(GQtWindow *gw, GEvent *event, int (*eh)(GWindow w, GEvent *)) {
    if (eh) {
        (eh)(gw->Base(), event);
    }
}


static GWindow _GQtDraw_CreateWindow(GQtDisplay *gdisp, GWindow w, GRect *pos,
                                      int (*eh)(GWindow, GEvent *), void *user_data, GWindowAttrs *wattrs) {

    Qt::WindowFlags windowFlags = Qt::Widget;
    std::unique_ptr<GQtWindow> nw(new GQtWindow());
    GWindow ret = nw->Base();
    ret->native_window = nw.get();

    if (wattrs == nullptr) {
        static GWindowAttrs temp = GWINDOWATTRS_EMPTY;
        wattrs = &temp;
    }

    if (w == nullptr) { // Creating a top-level window. Set parent as default root.
        windowFlags |= Qt::Window;
    }

    // Now check window type
    if ((wattrs->mask & wam_nodecor) && wattrs->nodecoration) {
        // Is a modeless dialogue
        ret->is_popup = true;
        nw->is_dlg = true;
        windowFlags |= Qt::Popup; // hmm
    } else if ((wattrs->mask & wam_isdlg) && wattrs->is_dlg) {
        nw->is_dlg = true;
        windowFlags |= Qt::Dialog;
    }
    ret->is_toplevel = (windowFlags & Qt::Window) != 0;

    // Drawing context
    ret->ggc = _GQtDraw_NewGGC();

    // Base fields
    ret->display = gdisp->Base();
    ret->eh = eh;
    ret->parent = w;
    ret->pos = *pos;
    ret->user_data = user_data;

    QString title;
    QWidget *parent = nullptr;

    // Window title, hints
    if (ret->is_toplevel) {
        // Icon titles are ignored.
        if ((wattrs->mask & wam_utf8_wtitle) && (wattrs->utf8_window_title != nullptr)) {
            title = QString::fromUtf8(wattrs->utf8_window_title);
            nw->window_title = wattrs->utf8_window_title;
        }
        if (ret->is_popup || (wattrs->mask & wam_palette)) {
            windowFlags |= Qt::ToolTip;
        }
    } else {
        parent = GQtW(w)->Widget();
    }

    if (wattrs->mask & wam_restrict) {
        nw->restrict_input_to_me = wattrs->restrict_input_to_me;
    }

    std::unique_ptr<GQtWidget> window(new GQtWidget(nw.get(), parent, windowFlags));
    nw->q_base = window.get();

    window->resize(pos->width, pos->height);

    // We center windows here because we need to know the window size+decor
    // There is a bug on Windows (all versions < 3.21.1, <= 2.24.30) so don't use Qt_WA_X/Qt_WA_Y
    // https://bugzilla.gnome.org/show_bug.cgi?id=764996
    if (ret->is_toplevel && (!(wattrs->mask & wam_positioned) || (wattrs->mask & wam_centered))) {
        nw->is_centered = true;
        // _GQtDraw_CenterWindowOnScreen(nw);
    } else {
        window->move(ret->pos.x, ret->pos.y);
    }

    // Set background
    if (!(wattrs->mask & wam_backcol) || wattrs->background_color == COLOR_DEFAULT) {
        wattrs->background_color = _GDraw_res_bg;
    }
    ret->ggc->bg = wattrs->background_color;
    GQtDrawSetWindowBackground(ret, wattrs->background_color);

    if (ret->is_toplevel) {
        // Set icon
        GQtWindow *icon = gdisp->default_icon;
        if (((wattrs->mask & wam_icon) && wattrs->icon != nullptr) && wattrs->icon->is_pixmap) {
            icon = GQtW(wattrs->icon);
        }
        if (icon != nullptr) {
            window->setWindowIcon(QIcon(*icon->Pixmap()));
        } else {
            // Qt_window_set_decorations(nw->w, Qt_DECOR_ALL | Qt_DECOR_MENU);
        }

        if (wattrs->mask & wam_palette) {
            window->setBaseSize(window->size());
            window->setMinimumSize(window->size());
        }
        if ((wattrs->mask & wam_noresize) && wattrs->noresize) {
            window->setFixedSize(window->size());
        }
        nw->was_positioned = true;

        if ((wattrs->mask & wam_transient) && wattrs->transient != nullptr) {
            GQtDrawSetTransientFor(ret, wattrs->transient);
            nw->is_dlg = true;
        } else if (!nw->is_dlg) {
            ++gdisp->top_window_count;
        }
        // else if (nw->restrict_input_to_me && gdisp->mru_windows->length > 0) {
        //     GQtDrawSetTransientFor(ret, (GWindow) - 1);
        // }
        nw->isverytransient = (wattrs->mask & wam_verytransient) ? 1 : 0;
    }

    if ((wattrs->mask & wam_cursor) && wattrs->cursor != ct_default) {
        GQtDrawSetCursor(ret, wattrs->cursor);
    }

    // Event handler
    if (eh != nullptr) {
        GEvent e = {};
        e.type = et_create;
        e.w = ret;
        e.native_window = nw.get();
        _GQtDraw_CallEHChecked(nw.get(), &e, eh);
    }

    Log(LOGDEBUG, "Window created: %p[%p][%s][toplevel:%d]", nw.get(), nw->Widget(), nw->window_title.c_str(), ret->is_toplevel);
    window.release();
    nw.release();
    return ret;
}

static GWindow _GQtDraw_NewPixmap(GDisplay *disp, GWindow similar, uint16 width, uint16 height, bool is_bitmap,
                                   const unsigned char *data) {
    std::unique_ptr<GQtWindow> gw(new GQtWindow());
    std::unique_ptr<GQtPixmap> pixmap(new GQtPixmap(width, height));
    GWindow ret = gw->Base();
    QImage::Format format;
    int stride;

    ret->ggc = _GQtDraw_NewGGC();
    ret->ggc->bg = _GDraw_res_bg;
    width &= 0x7fff;

    ret->native_window = gw.get();
    ret->display = disp;
    ret->is_pixmap = 1;
    ret->parent = nullptr;
    ret->pos.x = ret->pos.y = 0;
    ret->pos.width = width;
    ret->pos.height = height;
    gw->q_base = pixmap.get();

    if (data != nullptr) {
        if (is_bitmap) {
            QImage img(data, width, height, width / 8, QImage::Format_MonoLSB);
            QBitmap bm = QBitmap::fromImage(img);

            img.invertPixels();
            gw->Pixmap()->convertFromImage(img);
            gw->Pixmap()->setMask(bm);
        } else {
            gw->Pixmap()->convertFromImage(QImage(data, width, height, QImage::Format_ARGB32));
        }
    }

    pixmap.release();
    gw.release();
    return ret;
}


void GQtWidget::paintEvent(QPaintEvent *event) {
    Log(LOGDEBUG, "PAINTING %p %s", this->gwindow, this->gwindow->window_title.c_str());
    QPainter painter(this);
    this->painter = &painter;

    const QRect& rect = event->rect();
    GEvent gevent = {};
    gevent.w = this->gwindow->Base();
    gevent.native_window = this->gwindow;
    gevent.type = et_expose;

    gevent.u.expose.rect.x = rect.x();
    gevent.u.expose.rect.y = rect.y();
    gevent.u.expose.rect.width = rect.width();
    gevent.u.expose.rect.height = rect.height();

    this->gwindow->is_in_paint = true;
    _GQtDraw_CallEHChecked(this->gwindow, &gevent, gevent.w->eh);
    this->gwindow->is_in_paint = false;
    this->painter = nullptr;
}

void GQtWidget::configureEvent() {
    GEvent gevent = {};
    gevent.w = this->gwindow->Base();
    gevent.native_window = this->gwindow;
    gevent.type = et_resize;

    auto geom = geometry();

    gevent.u.resize.size.x      = geom.x();
    gevent.u.resize.size.y      = geom.y();
    gevent.u.resize.size.width  = geom.width();
    gevent.u.resize.size.height = geom.height();
    gevent.u.resize.dx          = geom.x() - gevent.w->pos.x;
    gevent.u.resize.dy          = geom.y() - gevent.w->pos.y;
    gevent.u.resize.dwidth      = geom.width() - gevent.w->pos.width;
    gevent.u.resize.dheight     = geom.height() - gevent.w->pos.height;
    gevent.u.resize.moved       = gevent.u.resize.sized = false;
    if (gevent.u.resize.dx != 0 || gevent.u.resize.dy != 0) {
        gevent.u.resize.moved = true;
        this->gwindow->is_centered = false;
    }
    if (gevent.u.resize.dwidth != 0 || gevent.u.resize.dheight != 0) {
        gevent.u.resize.sized = true;
    }

    gevent.w->pos = gevent.u.resize.size;

    // if (gevent.u.resize.sized || gevent.u.resize.moved) {
    //     return;
    // }

    // I could make this Windows specific... But it doesn't seem necessary on other platforms too.
    // On Windows, repeated configure messages are sent if we move the window around.
    // This causes CPU usage to go up because mouse handlers of this message just redraw the whole window.
    if (gevent.w->is_toplevel && !gevent.u.resize.sized && gevent.u.resize.moved) {
        Log(LOGDEBUG, "Configure DISCARDED: %p:%s, %d %d %d %d", gevent.w, this->gwindow->window_title, gevent.w->pos.x, gevent.w->pos.y, gevent.w->pos.width, gevent.w->pos.height);
        return;
    } else {
        Log(LOGDEBUG, "CONFIGURED: %p:%s, %d %d %d %d", gevent.w, this->gwindow->window_title, gevent.w->pos.x, gevent.w->pos.y, gevent.w->pos.width, gevent.w->pos.height);
    }

    _GQtDraw_CallEHChecked(this->gwindow, &gevent, gevent.w->eh);
}

void GQtWidget::resizeEvent(QResizeEvent *event) {
    configureEvent();
}

void GQtWidget::moveEvent(QMoveEvent *event) {
    configureEvent();
}

static void GQtDrawInit(GDisplay *disp) {
    disp->fontstate = new FState();
    // In inches, because that's how fonts are measured
    disp->fontstate->res = disp->res;
}

static void GQtDrawSetDefaultIcon(GWindow icon) {
    Log(LOGDEBUG, " ");
    assert(icon->is_pixmap);
    GQtD(icon)->default_icon = GQtW(icon);
}

static GWindow GQtDrawCreateTopWindow(GDisplay *disp, GRect *pos, int (*eh)(GWindow w, GEvent *), void *user_data,
                                       GWindowAttrs *gattrs) {
    Log(LOGDEBUG, " ");
    return _GQtDraw_CreateWindow(GQtD(disp), nullptr, pos, eh, user_data, gattrs);
}

static GWindow GQtDrawCreateSubWindow(GWindow w, GRect *pos, int (*eh)(GWindow w, GEvent *), void *user_data,
                                       GWindowAttrs *gattrs) {
    Log(LOGDEBUG, " ");
    return _GQtDraw_CreateWindow(GQtD(w), w, pos, eh, user_data, gattrs);
}

static GWindow GQtDrawCreatePixmap(GDisplay *disp, GWindow similar, uint16 width, uint16 height) {
    Log(LOGDEBUG, " ");
    return _GQtDraw_NewPixmap(disp, similar, width, height, false, nullptr);
}

static GWindow GQtDrawCreateBitmap(GDisplay *disp, uint16 width, uint16 height, uint8 *data) {
    Log(LOGDEBUG, " ");
    return _GQtDraw_NewPixmap(disp, nullptr, width, height, true, data);
}

static GCursor GQtDrawCreateCursor(GWindow src, GWindow mask, Color fg, Color bg, int16_t x, int16_t y) {
    Log(LOGDEBUG, " ");

    GQtDisplay *gdisp = GQtD(src);
    if (mask == nullptr) { // Use src directly
        assert(src != nullptr);
        assert(src->is_pixmap);
        gdisp->custom_cursors.emplace_back(*GQtW(src)->Pixmap(), x, y);
    } else { // Assume it's an X11-style cursor
        QPixmap pixmap(src->pos.width, src->pos.height);

        // Masking
        //Background
        QBitmap bgMask(*GQtW(mask)->Pixmap());
        QBitmap fgMask(*GQtW(src)->Pixmap());
        pixmap.setMask(bgMask);

        QPainter painter(&pixmap);
        painter.fillRect(pixmap.rect(), QBrush(QColor(bg)));
        painter.end();

        pixmap.setMask(QBitmap());
        pixmap.setMask(fgMask);
        painter.begin(&pixmap);
        painter.fillRect(pixmap.rect(), QBrush(QColor(fg)));
        painter.end();

        gdisp->custom_cursors.emplace_back(pixmap, x, y);
    }

    return (GCursor)(ct_user + (gdisp->custom_cursors.size() - 1));
}

static void GQtDrawDestroyCursor(GDisplay *disp, GCursor gcursor) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawDestroyWindow(GWindow w) {
    Log(LOGDEBUG, " ");
}

static int GQtDrawNativeWindowExists(GDisplay *UNUSED(gdisp), void *native_window) {
    Log(LOGDEBUG, " ");
    return true;
}

static void GQtDrawSetZoom(GWindow UNUSED(gw), GRect *UNUSED(size), enum gzoom_flags UNUSED(flags)) {
    //Log(LOGDEBUG, " ");
    // Not implemented.
}

static void GQtDrawSetWindowBackground(GWindow w, Color gcol) {
    Log(LOGDEBUG, " ");
    GQtWindow *gw = GQtW(w);
    QPalette pal = QPalette();
    pal.setColor(QPalette::Window, QColor(gcol));
    gw->Widget()->setAutoFillBackground(true);
    gw->Widget()->setPalette(pal);
}

static int GQtDrawSetDither(GDisplay *UNUSED(gdisp), int UNUSED(set)) {
    // Not implemented; does nothing.
    return false;
}

static void GQtDrawSetVisible(GWindow w, int show) {
    Log(LOGDEBUG, "0x%p %d", w, show);
    GQtW(w)->Widget()->setVisible((bool)show);
}

static void GQtDrawMove(GWindow w, int32 x, int32 y) {
    Log(LOGDEBUG, "%p:%s, %d %d", w, GQtW(w)->window_title.c_str(), x, y);
    GQtW(w)->Widget()->move(x, y);
}

static void GQtDrawTrueMove(GWindow w, int32 x, int32 y) {
    Log(LOGDEBUG, " ");
    GQtW(w)->Widget()->move(x, y);
}

static void GQtDrawResize(GWindow w, int32 width, int32 height) {
    Log(LOGDEBUG, "%p:%s, %d %d", w, GQtW(w)->window_title.c_str(), width, height);
    GQtW(w)->Widget()->resize(width, height);
}

static void GQtDrawMoveResize(GWindow w, int32 x, int32 y, int32 width, int32 height) {
    Log(LOGDEBUG, "%p:%s, %d %d %d %d", w, GQtW(w)->window_title.c_str(), x, y, width, height);
    GQtW(w)->Widget()->setGeometry(x, y, width, height);
}

static void GQtDrawRaise(GWindow w) {
    Log(LOGDEBUG, "%p", w, GQtW(w)->window_title.c_str());
    GQtW(w)->Widget()->raise();
}

// Icon title is ignored.
static void GQtDrawSetWindowTitles8(GWindow w, const char *title, const char *UNUSED(icontitle)) {
    Log(LOGDEBUG, " ");// assert(false);
    auto* gw = GQtW(w);
    gw->Widget()->setWindowTitle(QString::fromUtf8(title));
    gw->window_title = title;
}

static char *GQtDrawGetWindowTitle8(GWindow w) {
    Log(LOGDEBUG, " ");
    return copy(GQtW(w)->window_title.c_str());
}

static void GQtDrawSetTransientFor(GWindow transient, GWindow owner) {
    Log(LOGDEBUG, "transient=%p, owner=%p", transient, owner);
    assert(transient->is_toplevel);
    assert(owner->is_toplevel);

    QWidget *trans = GQtW(transient)->Widget();
    QWidget *parent = GQtW(owner)->Widget();
    Qt::WindowFlags flags = trans->windowFlags();
    bool visible = trans->isVisible();

    trans->setParent(parent);
    trans->setWindowFlags(flags);
    if (visible) {
        trans->show();
    }
}

static void GQtDrawGetPointerPosition(GWindow w, GEvent *ret) {
    Log(LOGDEBUG, " ");
    auto *gdisp = GQtD(w);
    Qt::KeyboardModifiers modifiers = gdisp->app->keyboardModifiers();
    QPoint pos = QCursor::pos();

    ret->u.mouse.x = pos.x();
    ret->u.mouse.y = pos.y();
    ret->u.mouse.state = _GQtDraw_QtModifierToKsm(modifiers);
}

static GWindow GQtDrawGetPointerWindow(GWindow w) {
    Log(LOGDEBUG, " ");
    auto *gdisp = GQtD(w);
    GQtWidget *widget = dynamic_cast<GQtWidget*>(gdisp->app->widgetAt(QCursor::pos()));
    if (widget) {
        return widget->gwindow->Base();
    }
    return nullptr;
}

static void GQtDrawSetCursor(GWindow w, GCursor gcursor) {
    Log(LOGDEBUG, " ");

    QCursor cursor;
    switch (gcursor) {
        case ct_default:
        case ct_backpointer:
        case ct_pointer:
            break;
        case ct_hand:
            cursor = QCursor(Qt::OpenHandCursor);
            break;
        case ct_question:
            cursor = QCursor(Qt::WhatsThisCursor);
            break;
        case ct_cross:
            cursor = QCursor(Qt::CrossCursor);
            break;
        case ct_4way:
            cursor = QCursor(Qt::SizeAllCursor);
            break;
        case ct_text:
            cursor = QCursor(Qt::IBeamCursor);
            break;
        case ct_watch:
            cursor = QCursor(Qt::WaitCursor);
            break;
        case ct_draganddrop:
            cursor = QCursor(Qt::DragMoveCursor);
            break;
        case ct_invisible:
            return; // There is no *good* reason to make the cursor invisible
            break;
        default:
            Log(LOGDEBUG, "CUSTOM CURSOR! %d", gcursor);
    }

    auto *gw = GQtW(w);
    if (gcursor >= ct_user) {
        GQtDisplay *gdisp = GQtD(w);
        gcursor = (GCursor)(gcursor - ct_user);
        if ((size_t)gcursor < gdisp->custom_cursors.size()) {
            gw->Widget()->setCursor(gdisp->custom_cursors[gcursor]);
            gw->current_cursor = (GCursor)(gcursor + ct_user);
        } else {
            Log(LOGWARN, "Invalid cursor value passed: %d", gcursor);
        }
    } else {
        gw->Widget()->setCursor(cursor);
        gw->current_cursor = gcursor;
    }
}

static GCursor GQtDrawGetCursor(GWindow w) {
    Log(LOGDEBUG, " ");
    return GQtW(w)->current_cursor;
}

static void GQtDrawTranslateCoordinates(GWindow from, GWindow to, GPoint *pt) {
    Log(LOGDEBUG, " ");

    GQtWindow *gfrom = GQtW(from), *gto = GQtW(to);
    QPoint res;

    if (to == from->display->groot) {
        // The actual meaning of this command...
        res = gfrom->Widget()->mapToGlobal(QPoint(pt->x, pt->y));
    } else {
        res = gfrom->Widget()->mapTo(gto->Widget(), QPoint(pt->x, pt->y));
    }

    pt->x = res.x();
    pt->y = res.y();
}

static void GQtDrawBeep(GDisplay *disp) {
    Log(LOGDEBUG, " ");
    GQtD(disp)->app->beep();
}

static void GQtDrawScroll(GWindow w, GRect *rect, int32 hor, int32 vert) {
    Log(LOGDEBUG, " ");
    GRect temp;

    vert = -vert;
    if (rect == nullptr) {
        temp.x = temp.y = 0;
        temp.width = w->pos.width;
        temp.height = w->pos.height;
        rect = &temp;
    }

    GDrawRequestExpose(w, rect, false);
}


static GIC *GQtDrawCreateInputContext(GWindow UNUSED(gw), enum gic_style UNUSED(style)) {
    Log(LOGDEBUG, " ");
    return nullptr;
}

static void GQtDrawSetGIC(GWindow UNUSED(gw), GIC *UNUSED(gic), int UNUSED(x), int UNUSED(y)) {
    Log(LOGDEBUG, " ");
}

static int GQtDrawKeyState(GWindow w, int keysym) {
    Log(LOGDEBUG, " ");
    if (keysym != ' ') {
        Log(LOGWARN, "Cannot check state of unsupported character!");
        return 0;
    }
    return 0;
    // Since this function is only used to check the state of the space button
    // Don't bother with a full implementation...
    // return ((GQtWindow)w)->display->is_space_pressed;
}

static void GQtDrawGrabSelection(GWindow w, enum selnames sn) {
    Log(LOGDEBUG, " ");

    if ((int)sn < 0 || sn >= sn_max) {
        return;
    }
}

static void GQtDrawAddSelectionType(GWindow w, enum selnames sel, char *type, void *data, int32 cnt, int32 unitsize,
                                     void *gendata(void *, int32 *len), void freedata(void *)) {
    Log(LOGDEBUG, " ");
}

static void *GQtDrawRequestSelection(GWindow w, enum selnames sn, char *type_name, int32 *len) {
    return nullptr;
}

static int GQtDrawSelectionHasType(GWindow w, enum selnames sn, char *type_name) {
    Log(LOGDEBUG, " ");
    return false;
}

static void GQtDrawBindSelection(GDisplay *disp, enum selnames sn, char *atomname) {
    Log(LOGDEBUG, " ");
}

static int GQtDrawSelectionHasOwner(GDisplay *disp, enum selnames sn) {
    Log(LOGDEBUG, " ");

    if ((int)sn < 0 || sn >= sn_max) {
        return false;
    }

    return false;
}

static void GQtDrawPointerUngrab(GDisplay *disp) {
    Log(LOGDEBUG, " ");
    GQtDisplay *gdisp = GQtD(disp);
    if (gdisp->grabbed_window != nullptr) {
        gdisp->grabbed_window->Widget()->releaseMouse();
    }
}

static void GQtDrawPointerGrab(GWindow w) {
    Log(LOGDEBUG, " ");
    GQtDisplay *gdisp = GQtD(w);
    GQtDrawPointerUngrab(gdisp->Base());
    gdisp->grabbed_window = GQtW(w);
    gdisp->grabbed_window->Widget()->grabMouse();
}

static void GQtDrawRequestExpose(GWindow w, GRect *rect, int UNUSED(doclear)) {
    Log(LOGDEBUG, "%p [%s]", w, GQtW(w)->window_title.c_str());

    GQtWindow *gw = GQtW(w);

    if (!w->is_visible || _GQtDraw_WindowOrParentsDying(gw->Base())) {
        return;
    }
    if (rect == nullptr) {
        gw->Widget()->update();
    } else {
        const GRect &pos = gw->Base()->pos;
        QRect clip;
        clip.setX(rect->x);
        clip.setY(rect->y);
        clip.setWidth(rect->width);
        clip.setHeight(rect->height);

        if (rect->x < 0 || rect->y < 0 || rect->x + rect->width > pos.width ||
                rect->y + rect->height > pos.height) {

            if (clip.x() < 0) {
                clip.setWidth(clip.width() + clip.x());
                clip.setX(0);
            }
            if (clip.y() < 0) {
                clip.setHeight(clip.height() + clip.y());
                clip.setY(0);
            }
            if (clip.x() + clip.width() > pos.width) {
                clip.setWidth(pos.width - clip.x());
            }
            if (clip.y() + clip.height() > pos.height) {
                clip.setHeight(pos.height - clip.y());
            }
            if (clip.height() <= 0 || clip.width() <= 0) {
                return;
            }
        }
        gw->Widget()->update(clip);
    }
}

static void GQtDrawForceUpdate(GWindow w) {
    Log(LOGDEBUG, " ");
    GQtD(w)->app->processEvents();
}

static void GQtDrawSync(GDisplay *disp) {
    // Log(LOGDEBUG, " ");
}

static void GQtDrawSkipMouseMoveEvents(GWindow UNUSED(gw), GEvent *UNUSED(gevent)) {
    //Log(LOGDEBUG, " ");
    // Not implemented, not needed.
}

static void GQtDrawProcessPendingEvents(GDisplay *disp) {
    //Log(LOGDEBUG, " ");
    GQtD(disp)->app->processEvents();
}

static void GQtDrawProcessOneEvent(GDisplay *disp) {
    //Log(LOGDEBUG, " ");
    GQtD(disp)->app->processEvents(QEventLoop::WaitForMoreEvents);
}

static void GQtDrawEventLoop(GDisplay *disp) {
    Log(LOGDEBUG, " ");
    GQtD(disp)->app->exec();
}

static void GQtDrawPostEvent(GEvent *e) {
    //Log(LOGDEBUG, " ");
    GQtWindow *gw = GQtW(e->w);
    e->native_window = gw;
    _GQtDraw_CallEHChecked(gw, e, e->w->eh);
}

static void GQtDrawPostDragEvent(GWindow w, GEvent *mouse, enum event_type et) {
    Log(LOGDEBUG, " ");
}

static int GQtDrawRequestDeviceEvents(GWindow w, int devcnt, struct gdeveventmask *de) {
    Log(LOGDEBUG, " ");
    return 0; //Not sure how to handle... For tablets...
}

static int GQtDrawShortcutKeyMatches(const GEvent *e, unichar_t ch) {
    return false;
}

GQtTimer::GQtTimer(GQtWindow *parent, void *userdata)
    : QTimer(parent->Widget())
    , gtimer{parent->Base(), this, userdata}
{
}

static GTimer *GQtDrawRequestTimer(GWindow w, int32 time_from_now, int32 frequency, void *userdata) {
    Log(LOGDEBUG, " ");
    GQtTimer *timer = new GQtTimer(GQtW(w), userdata);
    if (frequency == 0) {
        timer->setSingleShot(true);
    }

    QObject::connect(timer, &QTimer::timeout, [timer, frequency]{
        GEvent e = {};

        // if (_GQtDraw_WindowOrParentsDying((GQtWindow)timer->owner)) {
        //     return;
        // }

        e.type = et_timer;
        e.w = timer->Base()->owner;
        e.native_window = GQtW(e.w);
        e.u.timer.timer = timer->Base();
        e.u.timer.userdata = timer->Base()->userdata;

        _GQtDraw_CallEHChecked(GQtW(e.w), &e, e.w->eh);
        if (frequency) {
            timer->setInterval(frequency);
        }
    });

    timer->setInterval(time_from_now);
    timer->start();
    return timer->Base();
}

static void GQtDrawCancelTimer(GTimer *timer) {
    Log(LOGDEBUG, " ");
    GQtTimer *gtimer = (GQtTimer*)timer->impl;
    gtimer->stop();
    gtimer->deleteLater();
}


// DRAW RELATED


static QBrush GQtDraw_StippleMePink(int ts, Color fg) {
    static unsigned char grey_init[8] = {0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa};
    static unsigned char fence_init[8] = {0x55, 0x22, 0x55, 0x88, 0x55, 0x22, 0x55, 0x88};
    uint8 *spt;
    int bit, i, j;
    uint32 *data;
    uint32 space[8 * 8];

    if ((fg >> 24) != 0xff) {
        int alpha = fg >> 24, r = COLOR_RED(fg), g = COLOR_GREEN(fg), b = COLOR_BLUE(fg);
        r = (alpha * r + 128) / 255;
        g = (alpha * g + 128) / 255;
        b = (alpha * b + 128) / 255;
        fg = (alpha << 24) | (r << 16) | (g << 8) | b;
    }

    spt = (ts == 2) ? fence_init : grey_init;
    for (i = 0; i < 8; ++i) {
        data = space + 8 * i;
        for (j = 0, bit = 0x80; bit != 0; ++j, bit >>= 1) {
            if (spt[i]&bit) {
                data[j] = fg;
            } else {
                data[j] = 0;
            }
        }
    }

    QImage pattern((const unsigned char*)space, 8, 8, QImage::Format_ARGB32);
    return QBrush(pattern);
}

#if 0
static QImage _GQtDraw_GImage2QImage(GImage *image, GRect *src) {
    const struct _GImage *base = (image->list_len == 0) ? image->u.image : image->u.images[0];
    QImage::Format format;
    QImage ret;

    cairo_format_t type;
    uint8 *pt;
    uint32 *idata, *ipt, *ito;
    int i, j, jj, tjj, stride;
    int bit, tobit;
    cairo_surface_t *cs;

    if (base->image_type == it_rgba) {
        ret = QImage(base->data, base->width, base->height, base->bytes_per_line, QImage::Format_ARGB32);
    } else if (base->image_type == it_true && base->trans != COLOR_UNKNOWN) {
        ret = QImage(base->data, base->width, base->height, base->bytes_per_line, QImage::Format_RGB24);
        ret = ret.convertToFormat(QImage::Format_ARGB32);
        // apply trans...
    } else if (base->image_type == it_index && base->clut->trans_index != COLOR_UNKNOWN) {
        format = QImage::Format_ARGB32;
    } else if (base->image_type == it_true) {
        ret = QImage(base->data, base->width, base->height, base->bytes_per_line, QImage::Format_RGB24)
    } else if (base->image_type == it_mono && base->clut != NULL &&
               base->clut->trans_index != COLOR_UNKNOWN) {
        type = QImage::Format_MonoLSB;
    } else {
        format = QImage::Format_RGB32;
    }

    /* We can't reuse the image's data for alpha images because we must */
    /*  premultiply each channel by alpha. We can reuse it for non-transparent*/
    /*  rgb images */
    if (base->image_type == it_true && type == CAIRO_FORMAT_RGB24) {
        idata = ((uint32 *)(base->data + src->y * base->bytes_per_line)) + src->x;
        return cairo_image_surface_create_for_data((uint8 *) idata, type,
                src->width, src->height,
                base->bytes_per_line);
    }

    cs = cairo_image_surface_create(type, src->width, src->height);
    stride = cairo_image_surface_get_stride(cs);
    cairo_surface_flush(cs);
    idata = (uint32 *)cairo_image_surface_get_data(cs);

    if (base->image_type == it_rgba) {
        ipt = ((uint32 *)(base->data + src->y * base->bytes_per_line)) + src->x;
        ito = idata;
        for (i = 0; i < src->height; ++i) {
            for (j = 0; j < src->width; ++j) {
                uint32 orig = ipt[j];
                int alpha = orig >> 24;
                if (alpha == 0xff) {
                    ito[j] = orig;
                } else if (alpha == 0) {
                    ito[j] = 0x00000000;
                } else
                    ito[j] = (alpha << 24) |
                             ((COLOR_RED(orig) * alpha / 255) << 16) |
                             ((COLOR_GREEN(orig) * alpha / 255) << 8) |
                             ((COLOR_BLUE(orig) * alpha / 255));
            }
            ipt = (uint32 *)(((uint8 *) ipt) + base->bytes_per_line);
            ito = (uint32 *)(((uint8 *) ito) + stride);
        }
    } else if (base->image_type == it_true && base->trans != COLOR_UNKNOWN) {
        Color trans = base->trans;
        ipt = ((uint32 *)(base->data + src->y * base->bytes_per_line)) + src->x;
        ito = idata;
        for (i = 0; i < src->height; ++i) {
            for (j = 0; j < src->width; ++j) {
                if (ipt[j] == trans) {
                    ito[j] = 0x00000000;
                } else {
                    ito[j] = ipt[j] | 0xff000000;
                }
            }
            ipt = (uint32 *)(((uint8 *) ipt) + base->bytes_per_line);
            ito = (uint32 *)(((uint8 *) ito) + stride);
        }
    } else if (base->image_type == it_true) {
        ipt = ((uint32 *)(base->data + src->y * base->bytes_per_line)) + src->x;
        ito = idata;
        for (i = 0; i < src->height; ++i) {
            for (j = 0; j < src->width; ++j) {
                ito[j] = ipt[j] | 0xff000000;
            }
            ipt = (uint32 *)(((uint8 *) ipt) + base->bytes_per_line);
            ito = (uint32 *)(((uint8 *) ito) + stride);
        }
    } else if (base->image_type == it_index && base->clut->trans_index != COLOR_UNKNOWN) {
        int trans = base->clut->trans_index;
        Color *clut = base->clut->clut;
        pt = base->data + src->y * base->bytes_per_line + src->x;
        ito = idata;
        for (i = 0; i < src->height; ++i) {
            for (j = 0; j < src->width; ++j) {
                int index = pt[j];
                if (index == trans) {
                    ito[j] = 0x00000000;
                } else
                    /* In theory RGB24 images don't need the alpha channel set*/
                    /*  but there is a bug in Cairo 1.2, and they do. */
                {
                    ito[j] = clut[index] | 0xff000000;
                }
            }
            pt += base->bytes_per_line;
            ito = (uint32 *)(((uint8 *) ito) + stride);
        }
    } else if (base->image_type == it_index) {
        Color *clut = base->clut->clut;
        pt = base->data + src->y * base->bytes_per_line + src->x;
        ito = idata;
        for (i = 0; i < src->height; ++i) {
            for (j = 0; j < src->width; ++j) {
                int index = pt[j];
                ito[j] = clut[index] | 0xff000000;
            }
            pt += base->bytes_per_line;
            ito = (uint32 *)(((uint8 *) ito) + stride);
        }
#ifdef WORDS_BIGENDIAN
    } else if (base->image_type == it_mono && base->clut != NULL &&
               base->clut->trans_index != COLOR_UNKNOWN) {
        pt = base->data + src->y * base->bytes_per_line + (src->x >> 3);
        ito = idata;
        if (base->clut->trans_index == 0) {
            for (i = 0; i < src->height; ++i) {
                bit = (0x80 >> (src->x & 0x7));
                tobit = 0x80000000;
                for (j = jj = tjj = 0; j < src->width; ++j) {
                    if (pt[jj]&bit) {
                        ito[tjj] |= tobit;
                    }
                    if ((bit >>= 1) == 0) {
                        bit = 0x80;
                        ++jj;
                    }
                    if ((tobit >>= 1) == 0) {
                        tobit = 0x80000000;
                        ++tjj;
                    }
                }
                pt += base->bytes_per_line;
                ito = (uint32 *)(((uint8 *) ito) + stride);
            }
        } else {
            for (i = 0; i < src->height; ++i) {
                bit = (0x80 >> (src->x & 0x7));
                tobit = 0x80000000;
                for (j = jj = tjj = 0; j < src->width; ++j) {
                    if (!(pt[jj]&bit)) {
                        ito[tjj] |= tobit;
                    }
                    if ((bit >>= 1) == 0) {
                        bit = 0x80;
                        ++jj;
                    }
                    if ((tobit >>= 1) == 0) {
                        tobit = 0x80000000;
                        ++tjj;
                    }
                }
                pt += base->bytes_per_line;
                ito = (uint32 *)(((uint8 *) ito) + stride);
            }
        }
#else
    } else if (base->image_type == it_mono && base->clut != NULL &&
               base->clut->trans_index != COLOR_UNKNOWN) {
        pt = base->data + src->y * base->bytes_per_line + (src->x >> 3);
        ito = idata;
        if (base->clut->trans_index == 0) {
            for (i = 0; i < src->height; ++i) {
                bit = (0x80 >> (src->x & 0x7));
                tobit = 1;
                for (j = jj = tjj = 0; j < src->width; ++j) {
                    if (pt[jj]&bit) {
                        ito[tjj] |= tobit;
                    }
                    if ((bit >>= 1) == 0) {
                        bit = 0x80;
                        ++jj;
                    }
                    if ((tobit <<= 1) == 0) {
                        tobit = 0x1;
                        ++tjj;
                    }
                }
                pt += base->bytes_per_line;
                ito = (uint32 *)(((uint8 *) ito) + stride);
            }
        } else {
            for (i = 0; i < src->height; ++i) {
                bit = (0x80 >> (src->x & 0x7));
                tobit = 1;
                for (j = jj = tjj = 0; j < src->width; ++j) {
                    if (!(pt[jj]&bit)) {
                        ito[tjj] |= tobit;
                    }
                    if ((bit >>= 1) == 0) {
                        bit = 0x80;
                        ++jj;
                    }
                    if ((tobit <<= 1) == 0) {
                        tobit = 0x1;
                        ++tjj;
                    }
                }
                pt += base->bytes_per_line;
                ito = (uint32 *)(((uint8 *) ito) + stride);
            }
        }
#endif
    } else {
        Color fg = base->clut == NULL ? 0xffffff : base->clut->clut[1];
        Color bg = base->clut == NULL ? 0x000000 : base->clut->clut[0];
        /* In theory RGB24 images don't need the alpha channel set*/
        /*  but there is a bug in Cairo 1.2, and they do. */
        fg |= 0xff000000;
        bg |= 0xff000000;
        pt = base->data + src->y * base->bytes_per_line + (src->x >> 3);
        ito = idata;
        for (i = 0; i < src->height; ++i) {
            bit = (0x80 >> (src->x & 0x7));
            for (j = jj = 0; j < src->width; ++j) {
                ito[j] = (pt[jj] & bit) ? fg : bg;
                if ((bit >>= 1) == 0) {
                    bit = 0x80;
                    ++jj;
                }
            }
            pt += base->bytes_per_line;
            ito = (uint32 *)(((uint8 *) ito) + stride);
        }
    }
    cairo_surface_mark_dirty(cs);
    return cs;
}
#endif

static void GQtDrawPushClip(GWindow w, GRect *rct, GRect *old) {
    Log(LOGDEBUG, " ");

    // Return the current clip, and intersect the current clip with the desired
    // clip to get the new clip.
    auto& clip = w->ggc->clip;

    *old = clip;
    clip = *rct;
    if (clip.x + clip.width > old->x + old->width) {
        clip.width = old->x + old->width - clip.x;
    }
    if (clip.y + clip.height > old->y + old->height) {
        clip.height = old->y + old->height - clip.y;
    }
    if (clip.x < old->x) {
        if (clip.width > (old->x - clip.x)) {
            clip.width -= (old->x - clip.x);
        } else {
            clip.width = 0;
        }
        clip.x = old->x;
    }
    if (clip.y < old->y) {
        if (clip.height > (old->y - clip.y)) {
            clip.height -= (old->y - clip.y);
        } else {
            clip.height = 0;
        }
        clip.y = old->y;
    }
    if (clip.height < 0 || clip.width < 0) {
        // Negative values mean large positive values, so if we want to clip
        //  to nothing force clip outside window
        clip.x = clip.y = -100;
        clip.height = clip.width = 1;
    }

    QPainter *painter = GQtW(w)->Painter();
    painter->save();
    painter->setClipRect(QRect(clip.x, clip.y, clip.width, clip.height), Qt::IntersectClip);
}

static void GQtDrawPopClip(GWindow w, GRect *old) {
    Log(LOGDEBUG, " ");
    if (old) {
        w->ggc->clip = *old;
    }
    QPainter *painter = GQtW(w)->Painter();
    painter->restore();
}

static QPen GQtDrawGetPen(GGC *mine) {
    Color fg = mine->fg;
    if ((fg >> 24) == 0) {
        fg |= 0xff000000;
    }

    QPen pen;
    pen.setWidth(std::max(1, (int)mine->line_width));

    if (mine->dash_len != 0) {
        pen.setDashPattern({(qreal)mine->dash_len, (qreal)mine->skip_len});
    }

    // I don't use line join/cap. On a screen with small line_width they are irrelevant
    if (mine->ts != 0) {
        pen.setBrush(GQtDraw_StippleMePink(mine->ts, fg));
    } else {
        pen.setColor(QColor(fg));
    }
    return pen;
}

static QBrush GQtDrawGetBrush(GGC *mine) {
    Color fg = mine->fg;
    if ((fg >> 24) == 0) {
        fg |= 0xff000000;
    }
    if (mine->ts != 0) {
        return GQtDraw_StippleMePink(mine->ts, fg);
    } else {
        return QBrush(QColor(fg));
    }
}

static QFont GQtDrawGetFont(GFont *font) {
    QFont fd;

    fd.setFamily(QString::fromUtf8(font->rq.utf8_family_name));
    fd.setStyle((font->rq.style & fs_italic) ?
                    QFont::StyleItalic : QFont::StyleNormal);

    if (font->rq.style & fs_smallcaps) {
        fd.setCapitalization(QFont::SmallCaps);
    }
    fd.setWeight(font->rq.weight);
    fd.setStretch((font->rq.style & fs_condensed) ? QFont::Condensed :
                    (font->rq.style & fs_extended) ? QFont::Expanded  :
                    QFont::Unstretched);

    if (font->rq.style & fs_vertical) {
        //FIXME: not sure this is the right thing
        fd.setHintingPreference(QFont::PreferVerticalHinting);
    }

    if (font->rq.point_size <= 0) {
        // Any negative (pixel) values should be converted when font opened
        GDrawIError("Bad point size for Pango");
    }

    // Or set pixel size??
    fd.setPointSize(font->rq.point_size);
    return fd;
}

static void GQtDrawDrawLine(GWindow w, int32 x, int32 y, int32 xend, int32 yend, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QPen pen = GQtDrawGetPen(w->ggc);
    if (pen.width() & 1) {
        path.moveTo(x + .5, y + .5);
        path.lineTo(xend + .5, yend + .5);
    } else {
        path.moveTo(x, y);
        path.lineTo(xend, yend);
    }

    GQtW(w)->Painter()->strokePath(path, pen);
}

static void GQtDrawDrawArrow(GWindow w, int32 x, int32 y, int32 xend, int32 yend, int16_t arrows, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QPen pen = GQtDrawGetPen(w->ggc);
    if (pen.width() & 1) {
        x += .5;
        y += .5;
        xend += .5;
        yend += .5;
    }

    const double head_angle = 0.5;
    double angle = atan2(yend - y, xend - x) + FF_PI;
    double length = sqrt((x - xend) * (x - xend) + (y - yend) * (y - yend));

    path.moveTo(x, y);
    path.lineTo(xend, yend);
    GQtW(w)->Painter()->strokePath(path, pen);

    if (length < 2) { //No point arrowing something so small
        return;
    } else if (length > 20) {
        length = 10;
    } else {
        length *= 2. / 3.;
    }

    QBrush brush = GQtDrawGetBrush(w->ggc);
    path.clear();
    path.moveTo(xend, yend);
    path.lineTo(xend + length * cos(angle - head_angle), yend + length * sin(angle - head_angle));
    path.lineTo(xend + length * cos(angle + head_angle), yend + length * sin(angle + head_angle));
    path.closeSubpath();
    GQtW(w)->Painter()->fillPath(path, brush);
}

static void GQtDrawDrawRect(GWindow w, GRect *rect, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QPen pen = GQtDrawGetPen(w->ggc);
    if (pen.width() & 1) {
        path.addRect(rect->x + .5, rect->y + .5, rect->width, rect->height);
    } else {
        path.addRect(rect->x, rect->y, rect->width, rect->height);
    }

    GQtW(w)->Painter()->strokePath(path, pen);
}

static void GQtDrawFillRect(GWindow w, GRect *rect, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QBrush brush = GQtDrawGetBrush(w->ggc);
    path.addRect(rect->x, rect->y, rect->width, rect->height);

    GQtW(w)->Painter()->fillPath(path, brush);
}

static void GQtDrawFillRoundRect(GWindow w, GRect *rect, int radius, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QBrush brush = GQtDrawGetBrush(w->ggc);
    path.addRoundedRect(rect->x, rect->y, rect->width, rect->height, radius, radius);

    GQtW(w)->Painter()->fillPath(path, brush);
}

static void GQtDrawDrawEllipse(GWindow w, GRect *rect, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QPen pen = GQtDrawGetPen(w->ggc);
    if (pen.width() & 1) {
        path.addEllipse(rect->x + .5, rect->y + .5, rect->width, rect->height);
    } else {
        path.addEllipse(rect->x, rect->y, rect->width, rect->height);
    }

    GQtW(w)->Painter()->strokePath(path, pen);
}

static void GQtDrawFillEllipse(GWindow w, GRect *rect, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QBrush brush = GQtDrawGetBrush(w->ggc);
    path.addEllipse(rect->x, rect->y, rect->width, rect->height);

    GQtW(w)->Painter()->fillPath(path, brush);
}

static void GQtDrawDrawArc(GWindow w, GRect *rect, int32 sangle, int32 eangle, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    // Leftover from XDrawArc: sangle/eangle in degrees*64.
    double start = sangle / 64., end = eangle / 64.;

    QPainterPath path;
    QPen pen = GQtDrawGetPen(w->ggc);
    if (pen.width() & 1) {
        path.arcMoveTo(rect->x + .5, rect->y + .5, rect->width, rect->height, start);
        path.arcTo(rect->x + .5, rect->y + .5, rect->width, rect->height, start, end);
    } else {
        path.arcMoveTo(rect->x, rect->y, rect->width, rect->height, start);
        path.arcTo(rect->x, rect->y, rect->width, rect->height, start, end);
    }

    GQtW(w)->Painter()->strokePath(path, pen);
}

static void GQtDrawDrawPoly(GWindow w, GPoint *pts, int16_t cnt, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QPen pen = GQtDrawGetPen(w->ggc);
    double off = (pen.width() & 1) ? .5 : 0;

    path.moveTo(pts[0].x + off, pts[0].y + off);
    for (int i = 1; i < cnt; ++i) {
        path.lineTo(pts[i].x + off, pts[i].y + off);
    }

    GQtW(w)->Painter()->strokePath(path, pen);
}

static void GQtDrawFillPoly(GWindow w, GPoint *pts, int16_t cnt, Color col) {
    Log(LOGDEBUG, " ");

    w->ggc->fg = col;

    QPainterPath path;
    QBrush brush = GQtDrawGetBrush(w->ggc);
    QPen pen = GQtDrawGetPen(w->ggc);

    path.moveTo(pts[0].x, pts[0].y);
    for (int i = 1; i < cnt; ++i) {
        path.lineTo(pts[i].x, pts[i].y);
    }
    path.closeSubpath();
    GQtW(w)->Painter()->fillPath(path, brush);

    pen.setWidth(1); // hmm
    path.clear();
    path.moveTo(pts[0].x + .5, pts[0].y + .5);
    for (int i = 1; i < cnt; ++i) {
        path.lineTo(pts[i].x + .5, pts[i].y + .5);
    }
    path.closeSubpath();
    GQtW(w)->Painter()->strokePath(path, pen);
}

static void GQtDrawDrawImage(GWindow w, GImage *image, GRect *src, int32 x, int32 y) {
    Log(LOGDEBUG, " ");

#if 0
    cairo_surface_t *is = _GGDKDraw_GImage2Surface(image, src), *cs = is;
    struct _GImage *base = (image->list_len == 0) ? image->u.image : image->u.images[0];

    if (cairo_image_surface_get_format(is) == CAIRO_FORMAT_A1) {
        /* No color info, just alpha channel */
        Color fg = base->clut->trans_index == 0 ? base->clut->clut[1] : base->clut->clut[0];
#ifdef GDK_WINDOWING_QUARTZ
        // The quartz backend cannot mask/render A1 surfaces directly
        // So render to intermediate ARGB32 surface first, then render that to screen
        cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, src->width, src->height);
        cairo_t *cc = cairo_create(cs);
        cairo_set_source_rgba(cc, COLOR_RED(fg) / 255.0, COLOR_GREEN(fg) / 255.0, COLOR_BLUE(fg) / 255.0, 1.0);
        cairo_mask_surface(cc, is, 0, 0);
        cairo_destroy(cc);
#else
        cairo_set_source_rgba(COLOR_RED(fg) / 255.0, COLOR_GREEN(fg) / 255.0, COLOR_BLUE(fg) / 255.0, 1.0);
        cairo_mask_surface(cs, x, y);
        cs = NULL;
#endif
    }

    if (cs != NULL) {
        cairo_set_source_surface(cs, x, y);
        cairo_rectangle(x, y, src->width, src->height);
        cairo_fill(gw->cc);

        if (cs != is) {
            cairo_surface_destroy(cs);
        }
    }
    /* Clear source and mask, in case we need to */
    cairo_new_path(gw->cc);
    cairo_set_source_rgba(0, 0, 0, 0);

    cairo_surface_destroy(is);
#endif
}

// What we really want to do is use the grey levels as an alpha channel
static void GQtDrawDrawGlyph(GWindow w, GImage *image, GRect *src, int32 x, int32 y) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawDrawImageMagnified(GWindow w, GImage *image, GRect *src, int32 x, int32 y, int32 width, int32 height) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawDrawPixmap(GWindow w, GWindow pixmap, GRect *src, int32 x, int32 y) {
    Log(LOGDEBUG, " ");

    GQtW(w)->Painter()->drawPixmap(x, y, *GQtW(pixmap)->Pixmap(), src->x, src->y, src->width, src->height);
}

static enum gcairo_flags GQtDrawHasCairo(GWindow UNUSED(w)) {
    Log(LOGDEBUG, " ");
    return gc_all;
}

static void GQtDrawPathStartNew(GWindow w) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawPathClose(GWindow w) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawPathMoveTo(GWindow w, double x, double y) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawPathLineTo(GWindow w, double x, double y) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawPathCurveTo(GWindow w, double cx1, double cy1, double cx2, double cy2, double x, double y) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawPathStroke(GWindow w, Color col) {
    Log(LOGDEBUG, " ");
    w->ggc->fg = col;
}

static void GQtDrawPathFill(GWindow w, Color col) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawPathFillAndStroke(GWindow w, Color fillcol, Color strokecol) {
    Log(LOGDEBUG, " ");
    // This function is unused, so it's unclear if it's implemented correctly.
}

static void GQtDrawStartNewSubPath(GWindow w) {
    Log(LOGDEBUG, " ");
}

static int GQtDrawFillRuleSetWinding(GWindow w) {
    Log(LOGDEBUG, " ");
    return 1;
}

static int GQtDrawDoText8(GWindow w, int32 x, int32 y, const char *text, int32 cnt, Color col, enum text_funcs drawit,
                    struct tf_arg *arg) {
    Log(LOGDEBUG, " ");

    struct font_instance *fi = w->ggc->fi;
    if (fi == nullptr || !text[0]) {
        return 0;
    }

    QFont fd = GQtDrawGetFont(fi);
    QString qtext = QString::fromUtf8(text);
    if (drawit == tf_drawit) {
        QRect rct(x, y, w->ggc->clip.width - x, w->ggc->clip.height - y);
        if (!rct.isValid()) {
            return 0;
        }
        QRect bounds;
        GQtW(w)->Painter()->drawText(rct, 0, qtext, &bounds);
        return bounds.width();
    } else if (drawit == tf_rect) {
        QTextLayout layout; // qt 5.13 supports these relative to the paint device...
        layout.setText(qtext);
        layout.setFont(fd);
        layout.beginLayout();

        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            memset(&arg->size, 0, sizeof(arg->size));
            return 0;
        } else {
            QFontMetrics metrics(fd);
            line.setLineWidth(w->ggc->clip.width - x);
            auto ink = line.naturalTextRect();
            auto rect = line.rect();
            arg->size.lbearing = ink.x() - rect.x();
            arg->size.rbearing = ink.x() + ink.width() - rect.x();
            arg->size.width = ink.width();
            arg->size.as = line.ascent();
            arg->size.ds = line.descent(); // leading?
            arg->size.fas = metrics.ascent();
            arg->size.fds = metrics.descent();
            return arg->size.width;
        }
    }

    QFontMetrics metrics(fd);
    return metrics.horizontalAdvance(qtext);
}

static void GQtDrawPushClipOnly(GWindow w) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawClipPreserve(GWindow w) {
    Log(LOGDEBUG, " ");
}

// PANGO LAYOUT
static void GQtDrawGetFontMetrics(GWindow w, GFont *fi, int *as, int *ds, int *ld) {
    Log(LOGDEBUG, " ");

    QFont fd = GQtDrawGetFont(fi);
    QFontMetrics fm(fd);

    *as = fm.ascent();
    *ds = fm.descent();
    *ld = 0;
}

static void GQtDrawLayoutInit(GWindow w, char *text, int cnt, GFont *fi) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawLayoutDraw(GWindow w, int32 x, int32 y, Color fg) {
    Log(LOGDEBUG, " ");
}

static void GQtDrawLayoutIndexToPos(GWindow w, int index, GRect *pos) {
    Log(LOGDEBUG, " ");
    *pos = {0};
}

static int GQtDrawLayoutXYToIndex(GWindow w, int x, int y) {
    Log(LOGDEBUG, " ");
    return 0;
}

static void GQtDrawLayoutExtents(GWindow w, GRect *size) {
    Log(LOGDEBUG, " ");
    *size = {0};
}

static void GQtDrawLayoutSetWidth(GWindow w, int width) {
    Log(LOGDEBUG, " ");
}

static int GQtDrawLayoutLineCount(GWindow w) {
    Log(LOGDEBUG, " ");
    return 0;
}

static int GQtDrawLayoutLineStart(GWindow w, int l) {
    Log(LOGDEBUG, " ");
    return 0;
}
// END PANGO LAYOUT


// END DRAW RELATED

}} // fontforge::gdraw

using namespace fontforge::gdraw;

// Our function VTable
static struct displayfuncs gqtfuncs = {
    GQtDrawInit,

    GQtDrawSetDefaultIcon,

    GQtDrawCreateTopWindow,
    GQtDrawCreateSubWindow,
    GQtDrawCreatePixmap,
    GQtDrawCreateBitmap,
    GQtDrawCreateCursor,
    GQtDrawDestroyWindow,
    GQtDrawDestroyCursor,
    GQtDrawNativeWindowExists, //Not sure what this is meant to do...
    GQtDrawSetZoom,
    GQtDrawSetWindowBackground,
    GQtDrawSetDither,

    GQtDrawSetVisible,
    GQtDrawMove,
    GQtDrawTrueMove,
    GQtDrawResize,
    GQtDrawMoveResize,
    GQtDrawRaise,
    GQtDrawSetWindowTitles8,
    GQtDrawGetWindowTitle8,
    GQtDrawSetTransientFor,
    GQtDrawGetPointerPosition,
    GQtDrawGetPointerWindow,
    GQtDrawSetCursor,
    GQtDrawGetCursor,
    GQtDrawTranslateCoordinates,

    GQtDrawBeep,

    GQtDrawPushClip,
    GQtDrawPopClip,

    GQtDrawDrawLine,
    GQtDrawDrawArrow,
    GQtDrawDrawRect,
    GQtDrawFillRect,
    GQtDrawFillRoundRect,
    GQtDrawDrawEllipse,
    GQtDrawFillEllipse,
    GQtDrawDrawArc,
    GQtDrawDrawPoly,
    GQtDrawFillPoly,
    GQtDrawScroll,

    GQtDrawDrawImage,
    GQtDrawDrawGlyph,
    GQtDrawDrawImageMagnified,
    GQtDrawDrawPixmap,

    GQtDrawCreateInputContext,
    GQtDrawSetGIC,
    GQtDrawKeyState,

    GQtDrawGrabSelection,
    GQtDrawAddSelectionType,
    GQtDrawRequestSelection,
    GQtDrawSelectionHasType,
    GQtDrawBindSelection,
    GQtDrawSelectionHasOwner,

    GQtDrawPointerUngrab,
    GQtDrawPointerGrab,
    GQtDrawRequestExpose,
    GQtDrawForceUpdate,
    GQtDrawSync,
    GQtDrawSkipMouseMoveEvents,
    GQtDrawProcessPendingEvents,
    GQtDrawProcessOneEvent,
    GQtDrawEventLoop,
    GQtDrawPostEvent,
    GQtDrawPostDragEvent,
    GQtDrawRequestDeviceEvents,
    GQtDrawShortcutKeyMatches,

    GQtDrawRequestTimer,
    GQtDrawCancelTimer,

    GQtDrawGetFontMetrics,

    GQtDrawHasCairo,
    GQtDrawPathStartNew,
    GQtDrawPathClose,
    GQtDrawPathMoveTo,
    GQtDrawPathLineTo,
    GQtDrawPathCurveTo,
    GQtDrawPathStroke,
    GQtDrawPathFill,
    GQtDrawPathFillAndStroke, // Currently unused

    GQtDrawLayoutInit,
    GQtDrawLayoutDraw,
    GQtDrawLayoutIndexToPos,
    GQtDrawLayoutXYToIndex,
    GQtDrawLayoutExtents,
    GQtDrawLayoutSetWidth,
    GQtDrawLayoutLineCount,
    GQtDrawLayoutLineStart,
    GQtDrawStartNewSubPath,
    GQtDrawFillRuleSetWinding,

    GQtDrawDoText8,

    GQtDrawPushClipOnly,
    GQtDrawClipPreserve
};

extern "C" GDisplay *_GQtDraw_CreateDisplay(char *displayname, int *argc, char ***argv) {
    LogInit();

    std::unique_ptr<GQtDisplay> gdisp(new GQtDisplay());
    gdisp->app.reset(new QApplication(*argc, *argv));

    GDisplay *ret = gdisp->Base();
    ret->impl = gdisp.get();
    ret->funcs = &gqtfuncs;

    std::unique_ptr<GQtWindow> groot(new GQtWindow());
    QRect screenGeom = gdisp->app->primaryScreen()->geometry();

    ret->res = gdisp->app->primaryScreen()->logicalDotsPerInch();

    ret->groot = groot->Base();
    ret->groot->ggc = _GQtDraw_NewGGC();
    ret->groot->display = (GDisplay*)gdisp.get();
    ret->groot->native_window = groot.get();
    ret->groot->pos.width = screenGeom.width();
    ret->groot->pos.height = screenGeom.height();
    ret->groot->is_toplevel = true;
    ret->groot->is_visible = true;

    (ret->funcs->init)(ret);
    _GDraw_InitError(ret);

    groot.release();
    gdisp.release();
    return ret;
}

extern "C" void _GQtDraw_DestroyDisplay(GDisplay *disp) {
    delete GQtD(disp);
}

#endif // FONTFORGE_CAN_USE_QT