/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* Copyright (C) 2018 Hans Petter Jansson
 *
 * This file is part of Chafa, a program that turns images into character art.
 *
 * Chafa is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chafa is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Chafa.  If not, see <http://www.gnu.org/licenses/>. */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <glib.h>
#include "chafa/chafa.h"
#include "chafa/chafa-private.h"

/* Maximum number of symbols in symbols[]. Used for statically allocated arrays */
#define SYMBOLS_MAX 256

/* Fixed point multiplier */
#define FIXED_MULT 16384

struct ChafaCanvasCell
{
    gunichar c;

    /* Colors can be either packed RGBA or index */
    guint32 fg_color;
    guint32 bg_color;
};

struct ChafaCanvas
{
    gint refs;

    gint width, height;
    gint width_pixels, height_pixels;
    ChafaPixel *pixels;
    ChafaCanvasCell *cells;
    guint have_alpha : 1;
    guint needs_clear : 1;
    ChafaColor fg_color;
    ChafaColor bg_color;

    ChafaCanvasConfig config;
};

typedef struct
{
    ChafaPixel fg;
    ChafaPixel bg;
    gint error;
}
SymbolEval;

/* pixels_out must point to CHAFA_SYMBOL_N_PIXELS-element array */
static void
fetch_canvas_pixel_block (ChafaCanvas *canvas, gint cx, gint cy, ChafaPixel *pixels_out)
{
    ChafaPixel *row_p;
    ChafaPixel *end_p;
    gint i = 0;

    row_p = &canvas->pixels [cy * CHAFA_SYMBOL_HEIGHT_PIXELS * canvas->width_pixels + cx * CHAFA_SYMBOL_WIDTH_PIXELS];
    end_p = row_p + (canvas->width_pixels * CHAFA_SYMBOL_HEIGHT_PIXELS);

    for ( ; row_p < end_p; row_p += canvas->width_pixels)
    {
        ChafaPixel *p0 = row_p;
        ChafaPixel *p1 = p0 + CHAFA_SYMBOL_WIDTH_PIXELS;

        for ( ; p0 < p1; p0++)
            pixels_out [i++] = *p0;
    }
}

static void
calc_mean_color (ChafaPixel *pixels, ChafaColor *color_out)
{
    ChafaColor accum = { 0 };
    gint i;

    for (i = 0; i < CHAFA_SYMBOL_N_PIXELS; i++)
    {
        chafa_color_add (&accum, &pixels->col);
        pixels++;
    }

    chafa_color_div_scalar (&accum, CHAFA_SYMBOL_N_PIXELS);
    *color_out = accum;
}

#ifdef HAVE_MMX_INTRINSICS
# define calc_colors_faster calc_colors_mmx
#else
# define calc_colors_faster calc_colors_plain
#endif

static void
calc_colors_plain (const ChafaPixel *pixels, ChafaColor *cols, const guint8 *cov)
{
    const gint16 *in_s16 = (const gint16 *) pixels;
    gint i;

    for (i = 0; i < CHAFA_SYMBOL_N_PIXELS; i++)
    {
        gint16 *out_s16 = (gint16 *) (cols + *cov++);

        *out_s16++ += *in_s16++;
        *out_s16++ += *in_s16++;
        *out_s16++ += *in_s16++;
        *out_s16++ += *in_s16++;
    }
}

static void
eval_symbol_colors (const ChafaPixel *canvas_pixels, const ChafaSymbol *sym, SymbolEval *eval)
{
    const guint8 *covp = (guint8 *) &sym->coverage [0];
    ChafaColor cols [11] = { 0 };
    gint i;

#ifdef HAVE_MMX_INTRINSICS
    if (chafa_have_mmx ())
        calc_colors_mmx (canvas_pixels, cols, covp);
    else
#endif
        calc_colors_plain (canvas_pixels, cols, covp);

    eval->fg.col = cols [10];
    eval->bg.col = cols [0];

    if (sym->have_mixed)
    {
        for (i = 1; i < 10; i++)
        {
            eval->fg.col.ch [0] += ((gint) cols [i].ch [0] * 100 * (gint) i) / 1000;
            eval->fg.col.ch [1] += ((gint) cols [i].ch [1] * 100 * (gint) i) / 1000;
            eval->fg.col.ch [2] += ((gint) cols [i].ch [2] * 100 * (gint) i) / 1000;
            eval->fg.col.ch [3] += ((gint) cols [i].ch [3] * 100 * (gint) i) / 1000;

            eval->bg.col.ch [0] += ((gint) cols [i].ch [0] * 100 * (gint) (10 - i)) / 1000;
            eval->bg.col.ch [1] += ((gint) cols [i].ch [1] * 100 * (gint) (10 - i)) / 1000;
            eval->bg.col.ch [2] += ((gint) cols [i].ch [2] * 100 * (gint) (10 - i)) / 1000;
            eval->bg.col.ch [3] += ((gint) cols [i].ch [3] * 100 * (gint) (10 - i)) / 1000;
        }
    }

    if (sym->fg_weight > 1)
        chafa_color_div_scalar (&eval->fg.col, (sym->fg_weight + 9) / 10);

    if (sym->bg_weight > 1)
        chafa_color_div_scalar (&eval->bg.col, (sym->bg_weight + 9) / 10);
}

static gint
calc_error_plain (const ChafaPixel *pixels, const ChafaColor *cols, const guint8 *cov)
{
    gint error = 0;
    gint i;

    for (i = 0; i < CHAFA_SYMBOL_N_PIXELS; i++)
    {
        guint8 p = *cov++;
        const ChafaPixel *p0 = pixels++;

        error += chafa_color_diff_fast (&cols [p], &p0->col, canvas->color_space);
    }

    return error;
}

static gint
calc_error_with_alpha (const ChafaPixel *pixels, const ChafaColor *cols, const guint8 *cov, ChafaColorSpace cs)
{
    gint error = 0;
    gint i;

    for (i = 0; i < CHAFA_SYMBOL_N_PIXELS; i++)
    {
        guint8 p = *cov++;
        const ChafaPixel *p0 = pixels++;

        error += chafa_color_diff_slow (&cols [p], &p0->col, cs);
    }

    return error;
}

static void
eval_symbol_error (ChafaCanvas *canvas, const ChafaPixel *canvas_pixels,
                   const ChafaSymbol *sym, SymbolEval *eval)
{
    const guint8 *covp = (guint8 *) &sym->coverage [0];
    ChafaColor cols [11] = { 0 };
    gint error;
    gint i;

    cols [0] = eval->bg.col;
    cols [10] = eval->fg.col;

    if (sym->have_mixed)
    {
        for (i = 1; i < 10; i++)
        {
            chafa_color_mix (&cols [i], &eval->fg.col, &eval->bg.col, i * 1000);
        }
    }

    if (canvas->have_alpha)
    {
        error = calc_error_with_alpha (canvas_pixels, cols, covp, canvas->config.color_space);
    }
    else
    {
#ifdef HAVE_SSE41_INTRINSICS
        if (chafa_have_sse41 ())
            error = calc_error_sse41 (canvas_pixels, cols, covp);
        else
#endif
            error = calc_error_plain (canvas_pixels, cols, covp);
    }

    eval->error = error;
}

static void
pick_symbol_and_colors (ChafaCanvas *canvas, gint cx, gint cy,
                        gunichar *sym_out,
                        ChafaColor *fg_col_out,
                        ChafaColor *bg_col_out,
                        gint *error_out)
{
    ChafaPixel canvas_pixels [CHAFA_SYMBOL_N_PIXELS];
    SymbolEval eval [SYMBOLS_MAX] = { 0 };
    gint n;
    gint i;

    fetch_canvas_pixel_block (canvas, cx, cy, canvas_pixels);

    for (i = 0; chafa_symbols [i].c != 0; i++)
    {
        eval [i].error = G_MAXINT;

        /* Always evaluate space so we get fallback colors */
        if (chafa_symbols [i].sc != CHAFA_SYMBOL_CLASS_SPACE &&
            (!(chafa_symbols [i].sc & canvas->config.include_symbols)
             || (chafa_symbols [i].sc & canvas->config.exclude_symbols)))
            continue;

        if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_FGBG)
        {
            eval [i].fg.col = canvas->fg_color;
            eval [i].bg.col = canvas->bg_color;
        }
        else
        {
            ChafaColor fg_col, bg_col;

            eval_symbol_colors (canvas_pixels, &chafa_symbols [i], &eval [i]);

            /* Threshold alpha */

            if (eval [i].fg.col.ch [3] < canvas->config.alpha_threshold)
                eval [i].fg.col.ch [3] = 0x00;
            else
                eval [i].fg.col.ch [3] = 0xff;

            if (eval [i].bg.col.ch [3] < canvas->config.alpha_threshold)
                eval [i].bg.col.ch [3] = 0x00;
            else
                eval [i].bg.col.ch [3] = 0xff;

            fg_col = eval [i].fg.col;
            bg_col = eval [i].bg.col;

            /* Pick palette colors before error evaluation; this improves
             * fine detail fidelity slightly. */

            if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_16)
            {
                if (canvas->config.quality >= 5)
                {
                    fg_col = *chafa_get_palette_color_256 (chafa_pick_color_16 (&eval [i].fg.col,
                                                                                canvas->config.color_space), canvas->config.color_space);
                    bg_col = *chafa_get_palette_color_256 (chafa_pick_color_16 (&eval [i].bg.col,
                                                                                canvas->config.color_space), canvas->config.color_space);
                }
            }
            else if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_240)
            {
                if (canvas->config.quality >= 8)
                {
                    fg_col = *chafa_get_palette_color_256 (chafa_pick_color_240 (&eval [i].fg.col,
                                                                                 canvas->config.color_space), canvas->config.color_space);
                    bg_col = *chafa_get_palette_color_256 (chafa_pick_color_240 (&eval [i].bg.col,
                                                                                 canvas->config.color_space), canvas->config.color_space);
                }
            }
            else if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_256)
            {
                if (canvas->config.quality >= 8)
                {
                    fg_col = *chafa_get_palette_color_256 (chafa_pick_color_256 (&eval [i].fg.col,
                                                                                 canvas->config.color_space), canvas->config.color_space);
                    bg_col = *chafa_get_palette_color_256 (chafa_pick_color_256 (&eval [i].bg.col,
                                                                                 canvas->config.color_space), canvas->config.color_space);
                }
            }

            /* FIXME: The logic here seems overly complicated */
            if (canvas->config.canvas_mode != CHAFA_CANVAS_MODE_TRUECOLOR)
            {
                /* Transfer mean alpha over so we can use it later */

                fg_col.ch [3] = eval [i].fg.col.ch [3];
                bg_col.ch [3] = eval [i].bg.col.ch [3];

                eval [i].fg.col = fg_col;
                eval [i].bg.col = bg_col;
            }
        }

        eval_symbol_error (canvas, canvas_pixels, &chafa_symbols [i], &eval [i]);
    }

#ifdef HAVE_MMX_INTRINSICS
    /* Make FPU happy again */
    if (chafa_have_mmx ())
        leave_mmx ();
#endif

    if (error_out)
        *error_out = eval [0].error;

    for (i = 0, n = 0; chafa_symbols [i].c != 0; i++)
    {
        if (!(chafa_symbols [i].sc & canvas->config.include_symbols)
            || (chafa_symbols [i].sc & canvas->config.exclude_symbols))
            continue;

        if ((eval [i].fg.col.ch [0] != eval [i].bg.col.ch [0]
             || eval [i].fg.col.ch [1] != eval [i].bg.col.ch [1]
             || eval [i].fg.col.ch [2] != eval [i].bg.col.ch [2])
            && eval [i].error < eval [n].error)
        {
            n = i;
            if (error_out)
                *error_out = eval [i].error;
        }
    }

    /* If we couldn't find a symbol and space is excluded by user, try again
     * but allow symbols with equal fg/bg colors. */

    /* FIXME: Ugly duplicate code */

    if (!(chafa_symbols [n].sc & canvas->config.include_symbols)
        || (chafa_symbols [n].sc & canvas->config.exclude_symbols))
    {
        for (i = 0, n = -1; chafa_symbols [i].c != 0; i++)
        {
            if (!(chafa_symbols [i].sc & canvas->config.include_symbols)
                || (chafa_symbols [i].sc & canvas->config.exclude_symbols))
                continue;

            if (n < 0 || eval [i].error < eval [n].error)
            {
                n = i;
                if (error_out)
                    *error_out = eval [i].error;
            }
        }
    }

    /* Fall back to space */
    if (n < 0)
        n = 0;

    *sym_out = chafa_symbols [n].c;
    *fg_col_out = eval [n].fg.col;
    *bg_col_out = eval [n].bg.col;
}

static void
pick_fill_symbol_16 (ChafaCanvas *canvas, SymbolEval *eval, const ChafaColor *square_col,
                     const gint *sym_coverage, gint fg_col, gint bg_col,
                     SymbolEval *best_eval, gint *best_sym)
{
    gint i;
    gint n;

    for (i = 0; chafa_fill_symbols [i].c != 0; i++)
    {
        const ChafaSymbol *sym = &chafa_fill_symbols [i];
        SymbolEval *e = &eval [i];
        ChafaColor mixed_col;

        if (!(sym->sc & canvas->config.include_symbols)
            || (sym->sc & canvas->config.exclude_symbols))
            continue;

        e->fg.col = *chafa_get_palette_color_256 (fg_col, canvas->config.color_space);
        e->bg.col = *chafa_get_palette_color_256 (bg_col, canvas->config.color_space);

        chafa_color_mix (&mixed_col, &e->fg.col, &e->bg.col, sym_coverage [i]);

        if (canvas->have_alpha)
            e->error = chafa_color_diff_slow (&mixed_col, square_col, canvas->config.color_space);
        else
            e->error = chafa_color_diff_fast (&mixed_col, square_col, canvas->config.color_space);
    }

    for (i = 0, n = -1; chafa_fill_symbols [i].c != 0; i++)
    {
        const ChafaSymbol *sym = &chafa_fill_symbols [i];

        if (!(sym->sc & canvas->config.include_symbols)
            || (sym->sc & canvas->config.exclude_symbols))
            continue;

        if (n < 0 || eval [i].error < eval [n].error)
        {
            n = i;
        }
    }

    if (n >= 0 && eval [n].error < best_eval->error)
    {
        *best_eval = eval [n];
        *best_sym = n;
    }
}

static gboolean
pick_fill_16 (ChafaCanvas *canvas, gint cx, gint cy,
              gunichar *sym_out, ChafaColor *fg_col_out, ChafaColor *bg_col_out, gint *error_out)
{
    ChafaPixel canvas_pixels [CHAFA_SYMBOL_N_PIXELS];
    SymbolEval eval [SYMBOLS_MAX] = { 0 };
    SymbolEval best_eval = { 0 };
    gint fg_col, bg_col;
    gint best_sym = -1;
    ChafaColor square_col;
    gint sym_coverage [SYMBOLS_MAX] = { 0 };
    gint i;

    best_eval.error = G_MAXINT;

    fetch_canvas_pixel_block (canvas, cx, cy, canvas_pixels);
    calc_mean_color (canvas_pixels, &square_col);

    for (i = 0; chafa_fill_symbols [i].c != 0; i++)
    {
        const ChafaSymbol *sym = &chafa_fill_symbols [i];
        sym_coverage [i] = (sym->fg_weight * (gint) 100) / CHAFA_SYMBOL_N_PIXELS;
    }

    for (fg_col = 0; fg_col < 16; fg_col++)
    {
        for (bg_col = 0; bg_col < 16; bg_col++)
        {
            pick_fill_symbol_16 (canvas, eval, &square_col, sym_coverage, fg_col, bg_col,
                                 &best_eval, &best_sym);
        }

        pick_fill_symbol_16 (canvas, eval, &square_col, sym_coverage, fg_col, CHAFA_PALETTE_INDEX_TRANSPARENT,
                             &best_eval, &best_sym);
    }

    for (bg_col = 0; bg_col < 16; bg_col++)
    {
        pick_fill_symbol_16 (canvas, eval, &square_col, sym_coverage, CHAFA_PALETTE_INDEX_TRANSPARENT, bg_col,
                             &best_eval, &best_sym);
    }

    if (best_sym < 0)
        return FALSE;

    *sym_out = chafa_fill_symbols [best_sym].c;
    *fg_col_out = best_eval.fg.col;
    *bg_col_out = best_eval.bg.col;

    if (error_out)
        *error_out = best_eval.error;

    return TRUE;
}

static void
update_cells (ChafaCanvas *canvas)
{
    gint cx, cy;
    gint i = 0;

    for (cy = 0; cy < canvas->height; cy++)
    {
        for (cx = 0; cx < canvas->width; cx++, i++)
        {
            ChafaCanvasCell *cell = &canvas->cells [i];
            gunichar sym;
            ChafaColor fg_col, bg_col;

            pick_symbol_and_colors (canvas, cx, cy, &sym, &fg_col, &bg_col, NULL);
            cell->c = sym;

            if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_256)
            {
                cell->fg_color = chafa_pick_color_256 (&fg_col, canvas->config.color_space);
                cell->bg_color = chafa_pick_color_256 (&bg_col, canvas->config.color_space);
            }
            else if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_240)
            {
                cell->fg_color = chafa_pick_color_240 (&fg_col, canvas->config.color_space);
                cell->bg_color = chafa_pick_color_240 (&bg_col, canvas->config.color_space);
            }
            else if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_16)
            {
                cell->fg_color = chafa_pick_color_16 (&fg_col, canvas->config.color_space);
                cell->bg_color = chafa_pick_color_16 (&bg_col, canvas->config.color_space);

                /* Apply stipple symbols in solid cells (space or full block) */
                if ((cell->fg_color == cell->bg_color
                     || cell->c == 0x20
                     || cell->c == 0x2588 /* Full block */)
                    && pick_fill_16 (canvas, cx, cy, &sym, &fg_col, &bg_col, NULL))
                {
                    cell->c = sym;
                    cell->fg_color = chafa_pick_color_16 (&fg_col, canvas->config.color_space);
                    cell->bg_color = chafa_pick_color_16 (&bg_col, canvas->config.color_space);
                }
            }
            else if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_FGBG_BGFG)
            {
                cell->fg_color = chafa_pick_color_fgbg (&fg_col, canvas->config.color_space, &canvas->fg_color, &canvas->bg_color);
                cell->bg_color = chafa_pick_color_fgbg (&bg_col, canvas->config.color_space, &canvas->fg_color, &canvas->bg_color);
            }
            else
            {
                cell->fg_color = chafa_pack_color (&fg_col);
                cell->bg_color = chafa_pack_color (&bg_col);
            }
        }
    }
}

static void
multiply_alpha (ChafaCanvas *canvas)
{
    ChafaPixel *p0, *p1;

    p0 = canvas->pixels;
    p1 = p0 + canvas->width_pixels * canvas->height_pixels;

    for ( ; p0 < p1; p0++)
    {
        chafa_color_mix (&p0->col, &canvas->bg_color, &p0->col, 1000 - ((p0->col.ch [3] * 1000) / 255));
    }
}

static void
update_display_colors (ChafaCanvas *canvas)
{
    ChafaColor fg_col;
    ChafaColor bg_col;

    chafa_unpack_color (canvas->config.fg_color_packed_rgb, &fg_col);
    chafa_unpack_color (canvas->config.bg_color_packed_rgb, &bg_col);

    if (canvas->config.color_space == CHAFA_COLOR_SPACE_DIN99D)
    {
        chafa_color_rgb_to_din99d (&fg_col, &canvas->fg_color);
        chafa_color_rgb_to_din99d (&bg_col, &canvas->bg_color);
    }
    else
    {
        canvas->fg_color = fg_col;
        canvas->bg_color = bg_col;
    }
}

static void
rgba_to_internal_rgb (ChafaCanvas *canvas, const guint8 *data, gint width, gint height, gint rowstride)
{
    ChafaPixel *pixel;
    gint px, py;
    gint x_inc, y_inc;
    gint alpha_sum = 0;

    x_inc = (width * FIXED_MULT) / (canvas->width_pixels);
    y_inc = (height * FIXED_MULT) / (canvas->height_pixels);

    pixel = canvas->pixels;

    for (py = 0; py < canvas->height_pixels; py++)
    {
        const guint8 *data_row_p;

        data_row_p = data + ((py * y_inc) / FIXED_MULT) * rowstride;

        for (px = 0; px < canvas->width_pixels; px++)
        {
            const guint8 *data_p = data_row_p + ((px * x_inc) / FIXED_MULT) * 4;

            pixel->col.ch [0] = *data_p++;
            pixel->col.ch [1] = *data_p++;
            pixel->col.ch [2] = *data_p++;
            pixel->col.ch [3] = *data_p;

            alpha_sum += (0xff - pixel->col.ch [3]);

            pixel++;
        }
    }

    if (alpha_sum == 0)
    {
        canvas->have_alpha = FALSE;
    }
    else
    {
        canvas->have_alpha = TRUE;
        multiply_alpha (canvas);
    }
}

static void
rgba_to_internal_din99d (ChafaCanvas *canvas, const guint8 *data, gint width, gint height, gint rowstride)
{
    ChafaPixel *pixel;
    gint px, py;
    gint x_inc, y_inc;
    gint alpha_sum = 0;

    x_inc = (width * FIXED_MULT) / (canvas->width_pixels);
    y_inc = (height * FIXED_MULT) / (canvas->height_pixels);

    pixel = canvas->pixels;

    for (py = 0; py < canvas->height_pixels; py++)
    {
        const guint8 *data_row_p;

        data_row_p = data + ((py * y_inc) / FIXED_MULT) * rowstride;

        for (px = 0; px < canvas->width_pixels; px++)
        {
            const guint8 *data_p = data_row_p + ((px * x_inc) / FIXED_MULT) * 4;
            ChafaColor rgb_col;

            rgb_col.ch [0] = *data_p++;
            rgb_col.ch [1] = *data_p++;
            rgb_col.ch [2] = *data_p++;
            rgb_col.ch [3] = *data_p;

            alpha_sum += (0xff - rgb_col.ch [3]);

            chafa_color_rgb_to_din99d (&rgb_col, &pixel->col);

            pixel++;
        }
    }

    if (alpha_sum == 0)
    {
        canvas->have_alpha = FALSE;
    }
    else
    {
        canvas->have_alpha = TRUE;
        multiply_alpha (canvas);
    }
}

static void
maybe_clear (ChafaCanvas *canvas)
{
    gint i;

    if (!canvas->needs_clear)
        return;

    for (i = 0; i < canvas->width * canvas->height; i++)
    {
        ChafaCanvasCell *cell = &canvas->cells [i];

        memset (cell, 0, sizeof (*cell));
        cell->c = ' ';
    }
}

static GString *
build_ansi_gstring (ChafaCanvas *canvas)
{
    GString *gs = g_string_new ("");
    gint x, y;
    gint i = 0;

    maybe_clear (canvas);

    for (y = 0; y < canvas->height; y++)
    {
        for (x = 0; x < canvas->width; x++, i++)
        {
            ChafaCanvasCell *cell = &canvas->cells [i];

            if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_256
                || canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_240
                || canvas->config.canvas_mode == CHAFA_CANVAS_MODE_INDEXED_16)
            {
                /* FIXME: Use old school control codes for 16-color palette */

                if (cell->fg_color == CHAFA_PALETTE_INDEX_TRANSPARENT)
                {
                    if (cell->bg_color == CHAFA_PALETTE_INDEX_TRANSPARENT)
                    {
                        g_string_append (gs, "\x1b[0m ");
                    }
                    else
                    {
                        g_string_append_printf (gs, "\x1b[0m\x1b[7m\x1b[38;5;%dm%lc",
                                                cell->bg_color,
                                                (wint_t) cell->c);
                    }
                }
                else if (cell->bg_color == CHAFA_PALETTE_INDEX_TRANSPARENT)
                {
                    g_string_append_printf (gs, "\x1b[0m\x1b[38;5;%dm%lc",
                                            cell->fg_color,
                                            (wint_t) cell->c);
                }
                else
                {
                    g_string_append_printf (gs, "\x1b[0m\x1b[38;5;%dm\x1b[48;5;%dm%lc",
                                            cell->fg_color,
                                            cell->bg_color,
                                            (wint_t) cell->c);
                }
            }
            else if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_FGBG_BGFG)
            {
                g_string_append_printf (gs, "\x1b[%dm%lc",
                                        cell->bg_color == CHAFA_PALETTE_INDEX_FG ? 7 : 0,
                                        cell->fg_color == cell->bg_color ? ' ' : (wint_t) cell->c);
            }
            else if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_FGBG)
            {
                g_string_append_printf (gs, "%lc", (wint_t) cell->c);
            }
            else
            {
                ChafaColor fg, bg;

                chafa_unpack_color (cell->fg_color, &fg);
                chafa_unpack_color (cell->bg_color, &bg);

                if (fg.ch [3] < canvas->config.alpha_threshold)
                {
                    if (bg.ch [3] < canvas->config.alpha_threshold)
                    {
                        /* FIXME: Respect include/exclude for space */
                        g_string_append (gs, "\x1b[0m ");
                    }
                    else
                    {
                        g_string_append_printf (gs, "\x1b[0m\x1b[7m\x1b[38;2;%d;%d;%dm%lc",
                                                bg.ch [0], bg.ch [1], bg.ch [2],
                                                (wint_t) cell->c);
                    }
                }
                else if (bg.ch [3] < canvas->config.alpha_threshold)
                {
                    g_string_append_printf (gs, "\x1b[0m\x1b[38;2;%d;%d;%dm%lc",
                                            fg.ch [0], fg.ch [1], fg.ch [2],
                                            (wint_t) cell->c);
                }
                else
                {
                    g_string_append_printf (gs, "\x1b[0m\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm%lc",
                                            fg.ch [0], fg.ch [1], fg.ch [2],
                                            bg.ch [0], bg.ch [1], bg.ch [2],
                                            (wint_t) cell->c);
                }
            }
        }

        g_string_append (gs, "\x1b[0m\n");
    }

    return gs;
}

ChafaCanvas *
chafa_canvas_new (ChafaCanvasConfig *config, gint width, gint height)
{
    ChafaCanvas *canvas;

    g_return_val_if_fail (width > 0, NULL);
    g_return_val_if_fail (height > 0, NULL);

    chafa_init ();

    canvas = g_new0 (ChafaCanvas, 1);
    canvas->refs = 1;
    canvas->width_pixels = width * CHAFA_SYMBOL_WIDTH_PIXELS;
    canvas->height_pixels = height * CHAFA_SYMBOL_HEIGHT_PIXELS;
    canvas->pixels = g_new (ChafaPixel, canvas->width_pixels * canvas->height_pixels);
    canvas->width = width;
    canvas->height = height;
    canvas->cells = g_new (ChafaCanvasCell, width * height);
    canvas->needs_clear = TRUE;

    if (config)
        chafa_canvas_config_copy_contents (&canvas->config, config);
    else
        chafa_canvas_config_init (&canvas->config);

    /* In truecolor mode we don't support any fancy color spaces for now, since
     * we'd have to convert back to RGB space when emitting control codes, and
     * the code for that has yet to be written. In palette modes we just use
     * the palette mappings. */
    if (canvas->config.canvas_mode == CHAFA_CANVAS_MODE_TRUECOLOR)
        canvas->config.color_space = CHAFA_COLOR_SPACE_RGB;

    update_display_colors (canvas);

    return canvas;
}

ChafaCanvas *
chafa_canvas_new_similar (ChafaCanvas *orig)
{
    ChafaCanvas *canvas;

    g_return_val_if_fail (orig != NULL, NULL);

    canvas = g_new (ChafaCanvas, 1);
    memcpy (canvas, orig, sizeof (*canvas));
    canvas->refs = 1;

    canvas->pixels = g_new (ChafaPixel, canvas->width_pixels * canvas->height_pixels);
    canvas->cells = g_new (ChafaCanvasCell, orig->width * orig->height);
    canvas->needs_clear = TRUE;

    return canvas;
}

void
chafa_canvas_ref (ChafaCanvas *canvas)
{
    g_return_if_fail (canvas != NULL);
    g_return_if_fail (canvas->refs > 0);

    canvas->refs++;
}

void
chafa_canvas_unref (ChafaCanvas *canvas)
{
    g_return_if_fail (canvas != NULL);
    g_return_if_fail (canvas->refs > 0);

    if (--canvas->refs == 0)
    {
        g_free (canvas->pixels);
        g_free (canvas->cells);
        g_free (canvas);
    }
}

const ChafaCanvasConfig *
chafa_canvas_peek_config (ChafaCanvas *canvas)
{
    g_return_val_if_fail (canvas != NULL, NULL);
    g_return_val_if_fail (canvas->refs > 0, NULL);

    return &canvas->config;
}

void
chafa_canvas_set_contents_rgba (ChafaCanvas *canvas, guint8 *src_pixels,
                                gint src_width, gint src_height, gint src_rowstride)
{
    g_return_if_fail (canvas != NULL);
    g_return_if_fail (canvas->refs > 0);
    g_return_if_fail (src_pixels != NULL);
    g_return_if_fail (src_width >= 0 && src_width < 16384);
    g_return_if_fail (src_height >= 0 && src_height < 16384);
    g_return_if_fail (src_rowstride > 0);

    if (src_width == 0 || src_height == 0)
        return;

    switch (canvas->config.color_space)
    {
        case CHAFA_COLOR_SPACE_RGB:
            rgba_to_internal_rgb (canvas, src_pixels, src_width, src_height, src_rowstride);
            break;

        case CHAFA_COLOR_SPACE_DIN99D:
            rgba_to_internal_din99d (canvas, src_pixels, src_width, src_height, src_rowstride);
            break;
    }

    if (canvas->config.alpha_threshold == 0)
        canvas->have_alpha = FALSE;

    update_cells (canvas);
    canvas->needs_clear = FALSE;
}

GString *
chafa_canvas_build_ansi (ChafaCanvas *canvas)
{
    g_return_val_if_fail (canvas != NULL, NULL);
    g_return_val_if_fail (canvas->refs > 0, NULL);

    return build_ansi_gstring (canvas);
}
