/*
 * Copyright 2006, The Android Open Source Project
 * Portions created by Sony Ericsson are Copyright (C) 2010, 2011 Sony Ericsson Mobile Communications AB.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * This file has been modified by Sony Ericsson on 2010-12-12, 2011-12-19.
 */


#include "config.h"
#include "RenderSkinCombo.h"

#include "CString.h"
#include "Document.h"
#include "Element.h"
#include "Node.h"
#include "NodeRenderStyle.h"
#include "RenderStyle.h"
#include "SkCanvas.h"
#include "SkNinePatch.h"

namespace WebCore {

// Indicates if the entire asset is being drawn, or if the border is being
// excluded and just the arrow drawn.
enum BorderStyle {
    FullAsset,
    NoBorder
};

// There are 2.5 different concepts of a 'border' here, which results
// in rather a lot of magic constants. In each case, there are 2
// numbers, one for medium res and one for high-res. All sizes are in pixels.

// Firstly, we have the extra padding that webkit needs to know about,
// which defines how much bigger this element is made by the
// asset. This is actually a bit broader than the actual border on the
// asset, to make things look less cramped. The border is the same
// width on all sides, except on the right when it's significantly
// wider to allow for the arrow.
const int RenderSkinCombo::arrowMargin[3] = {22, 34, 42};
const int RenderSkinCombo::padMargin[3] = {2, 5, 2};

// Then we have the borders used for the 9-patch stretch.
// The rectangle at the centre of these borders is entirely to the left of the arrow
// in the asset. Hence the border widths are the same for the bottom, left and top.
// The right hand border width happens to be the same as arrowMargin defined above.
static const int        stretchMargin[3] = {3, 5, 8};   // Border width for the bottom, left and top, of the 9-patch

// Finally, if the border is defined by the CSS, we only draw the
// arrow and not the border. We do this by drawing the relevant subset
// of the bitmap, which must now be precisely determined by what's in
// the asset with no extra padding to make things look properly
// spaced. The border to remove at the top, right and bottom of the
// image is the same as stretchMargin above, but we need to know the width
// of the arrow.
static const int arrowWidth[3] = {21, 31, 42};

RenderSkinCombo::Resolution RenderSkinCombo::resolution = HighRes;

const SkIRect RenderSkinCombo::margin[3][2] = {{{ stretchMargin[MedRes], stretchMargin[MedRes],
                                          RenderSkinCombo::arrowMargin[MedRes] + stretchMargin[MedRes], stretchMargin[MedRes] },
                                        {0, stretchMargin[MedRes], 0, stretchMargin[MedRes]}},
                                       {{ stretchMargin[HighRes], stretchMargin[HighRes],
                                          RenderSkinCombo::arrowMargin[HighRes] + stretchMargin[HighRes], stretchMargin[HighRes] },
                                        {0, stretchMargin[HighRes], 0, stretchMargin[HighRes]}},
                                       {{ stretchMargin[XHighRes], stretchMargin[XHighRes],
                                          RenderSkinCombo::arrowMargin[XHighRes] + stretchMargin[XHighRes], stretchMargin[XHighRes] },
                                        {0, stretchMargin[XHighRes], 0, stretchMargin[XHighRes]}}};

static SkBitmap         bitmaps[2][2]; // Collection of assets for a combo box
static SkBitmap         arrowBitmaps[2]; // Collection of arrow-assets for a combo box
static bool             isDecoded;      // True if all assets were decoded

// Modification-values for the x and y coordinate of the combobox arrow. Measured on the image resources.
// In the form arrowCoordinateModification[resolution]
static SkScalar         arrowXCoordinateModification[3] = {SkIntToScalar(18), SkIntToScalar(26), SkIntToScalar(35)};
static SkScalar         arrowYCoordinateModification[3] = {SkIntToScalar(5), SkIntToScalar(7), SkIntToScalar(10)};

void RenderSkinCombo::Init(android::AssetManager* am, String drawableDirectory)
{
    if (isDecoded)
        return;

    if (!drawableDirectory.isEmpty()) {
        if (drawableDirectory.endsWith("-mdpi/")) {
            resolution = MedRes;
        }
        else if (drawableDirectory.endsWith("-xhdpi/")) {
            resolution = XHighRes;
        }
    }

    isDecoded = RenderSkinAndroid::DecodeBitmap(am, (drawableDirectory + "combobox_nohighlight.png").utf8().data(), &bitmaps[kNormal][FullAsset]);
    isDecoded &= RenderSkinAndroid::DecodeBitmap(am, (drawableDirectory + "combobox_disabled.png").utf8().data(), &bitmaps[kDisabled][FullAsset]);
    isDecoded &= RenderSkinAndroid::DecodeBitmap(am, (drawableDirectory + "combobox_arrow_nohighlight.png").utf8().data(), &arrowBitmaps[kNormal]);
    isDecoded &= RenderSkinAndroid::DecodeBitmap(am, (drawableDirectory + "combobox_arrow_disabled.png").utf8().data(), &arrowBitmaps[kDisabled]);

    int width = bitmaps[kNormal][FullAsset].width();
    int height = bitmaps[kNormal][FullAsset].height();
    SkIRect  subset;
    subset.set(width - arrowWidth[resolution], 0, width, height);
    bitmaps[kNormal][FullAsset].extractSubset(&bitmaps[kNormal][NoBorder], subset);
    bitmaps[kDisabled][FullAsset].extractSubset(&bitmaps[kDisabled][NoBorder], subset);
}

bool RenderSkinCombo::Draw(SkCanvas* canvas, Node* element, int x, int y, int width, int height)
{
    if (!isDecoded)
        return true;

    State state = (element->isElementNode() && static_cast<Element*>(element)->isEnabledFormControl()) ? kNormal : kDisabled;

    SkRect bounds;
    BorderStyle drawBorder = FullAsset;

    bounds.set(SkIntToScalar(x+1), SkIntToScalar(y+1), SkIntToScalar(x + width-1), SkIntToScalar(y + height-1));
    RenderStyle* style = element->renderStyle();
    SkPaint paint;
    paint.setColor(style->backgroundColor().rgb());
    canvas->drawRect(bounds, paint);

    bounds.set(SkIntToScalar(x), SkIntToScalar(y), SkIntToScalar(x + width), SkIntToScalar(y + height));

    if (style->borderLeftColor().isValid() ||
        style->borderRightColor().isValid() ||
        style->borderTopColor().isValid() ||
        style->borderBottomColor().isValid()) {
        bounds.fLeft += SkIntToScalar(width - arrowWidth[resolution] - style->borderRightWidth());
        bounds.fRight -= SkIntToScalar(style->borderRightWidth());
        bounds.fTop += SkIntToScalar(style->borderTopWidth());
        bounds.fBottom -= SkIntToScalar(style->borderBottomWidth());
        drawBorder = NoBorder;
    }

    SkScalar arrowX = bounds.fRight - arrowXCoordinateModification[resolution];
    SkScalar arrowY = bounds.fTop + SkScalarMul(bounds.fBottom - bounds.fTop, SK_ScalarHalf) - arrowYCoordinateModification[resolution];

    SkNinePatch::DrawNine(canvas, bounds, bitmaps[state][drawBorder], margin[resolution][drawBorder]);
    canvas->drawBitmap(arrowBitmaps[state], arrowX, arrowY);
    return false;
}

}   //WebCore
