/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "MuiCss.h"

using namespace Gdiplus;

/*
A css-like way to style controls/windows.

We define a bunch of css-like properties.

We have a Style, which is a logical group of properties.

Each control can have one or more styles that define how
a control looks like. A window has only one set of properties
but a button has several, one for each visual state of
the button (normal, on hover, pressed, default).

We define a bunch of default style so that if e.g. button
doesn't have style explicitly set, it'll get all the necessary
properties from our default set and have a consistent look.

Prop objects are never freed. To conserve memory, they are
internalized i.e. there are never 2 Prop objects with exactly
the same data.

TODO: Prop allocations should come from block allocator, so
that we don't pay OS's malloc() per-object overhead.
*/

namespace mui {
namespace css {

#define MKARGB(a, r, g, b) (((ARGB) (b)) | ((ARGB) (g) << 8) | ((ARGB) (r) << 16) | ((ARGB) (a) << 24))
#define MKRGB(r, g, b) (((ARGB) (b)) | ((ARGB) (g) << 8) | ((ARGB) (r) << 16) | ((ARGB)(0xff) << 24))

struct FontCacheEntry {
    Prop *fontName;
    Prop *fontSize;
    Prop *fontWeight;
    Font *font;

    // Prop objects are interned, so if the pointer is
    // the same, the value is the same too
    bool Eq(FontCacheEntry *other) const {
        return ((fontName == other->fontName) &&
                (fontSize == other->fontSize) &&
                (fontWeight == other->fontWeight));
    }
};

Vec<Prop*> *gAllProps = NULL;

Style *gStyleDefault = NULL;
Style *gStyleButtonDefault = NULL;
Style *gStyleButtonMouseOver = NULL;

static Vec<FontCacheEntry> *gCachedFonts = NULL;

void Initialize()
{
    CrashIf(gAllProps);
    gAllProps = new Vec<Prop*>();
    gCachedFonts = new Vec<FontCacheEntry>();

    // gDefaults is the very basic set shared by everyone
    gStyleDefault = new Style();
    gStyleDefault->Set(Prop::AllocFontName(L"Times New Roman"));
    gStyleDefault->Set(Prop::AllocFontSize(14));
    gStyleDefault->Set(Prop::AllocFontWeight(FontStyleBold));
    gStyleDefault->Set(Prop::AllocColorSolid(PropColor, "black"));
    //gDefaults->Set(Prop::AllocColorSolid(PropBgColor, 0xff, 0xff, 0xff));
    ARGB c1 = MKRGB(0xf5, 0xf6, 0xf6);
    ARGB c2 = MKRGB(0xe4, 0xe4, 0xe3);
    gStyleDefault->Set(Prop::AllocColorLinearGradient(PropBgColor, LinearGradientModeVertical, c1, c2));
    gStyleDefault->SetBorderWidth(1);
    gStyleDefault->SetBorderColor(MKRGB(0x99, 0x99, 0x99));
    gStyleDefault->Set(Prop::AllocColorSolid(PropBorderBottomColor, "#888"));
    gStyleDefault->Set(Prop::AllocPadding(0, 0, 0, 0));

    gStyleButtonDefault = new Style();
    gStyleButtonDefault->Set(Prop::AllocPadding(4, 8, 4, 8));
    gStyleButtonDefault->Set(Prop::AllocFontName(L"Lucida Grande"));
    gStyleButtonDefault->Set(Prop::AllocFontSize(8));
    gStyleButtonDefault->Set(Prop::AllocFontWeight(FontStyleBold));
    gStyleButtonDefault->inheritsFrom = gStyleDefault;

    gStyleButtonMouseOver = new Style();
    gStyleButtonMouseOver->Set(Prop::AllocColorSolid(PropBorderTopColor, "#777"));
    gStyleButtonMouseOver->Set(Prop::AllocColorSolid(PropBorderRightColor, "#777"));
    gStyleButtonMouseOver->Set(Prop::AllocColorSolid(PropBorderBottomColor, "#666"));
    //gStyleButtonMouseOver->Set(Prop::AllocColorSolid(PropBgColor, 180, 0, 0, 255));
    //gStyleButtonMouseOver->Set(Prop::AllocColorSolid(PropBgColor, "transparent"));
    gStyleButtonMouseOver->inheritsFrom = gStyleButtonDefault;
}

static void DeleteCachedFonts()
{
    for (size_t i = 0; i < gCachedFonts->Count(); i++) {
        FontCacheEntry c = gCachedFonts->At(i);
        ::delete c.font;
    }
    delete gCachedFonts;
}

void Destroy()
{
    DeleteVecMembers(*gAllProps);
    delete gAllProps;

    delete gStyleButtonDefault;
    delete gStyleButtonMouseOver;

    DeleteCachedFonts();
}

bool IsWidthProp(PropType type)
{
    return (PropBorderTopWidth == type) ||
           (PropBorderRightWidth == type) ||
           (PropBorderBottomWidth == type) ||
           (PropBorderLeftWidth == type);
}

bool IsColorProp(PropType type)
{
    return (PropColor == type) ||
           (PropBgColor == type) ||
           (PropBorderTopColor == type) ||
           (PropBorderRightColor == type) ||
           (PropBorderBottomColor == type) ||
           (PropBorderLeftColor == type);
}

// based on https://developer.mozilla.org/en/CSS/color_value
// TODO: add more colors
// TODO: change strings into linear string format, similar to how we store names
// html tags
static struct {
    const char *name;
    ARGB        color;
} gKnownColors[] = {
    { "black",  MKRGB(0, 0, 0) },
    { "white",  MKRGB(255,255,255) },
    { "gray",   MKRGB(128,128,128) },
    { "red",    MKRGB(255,0,0) },
    { "green",  MKRGB(0,128,0) },
    { "blue",   MKRGB(0,0,255) },
    { "transparent", MKARGB(0,0,0,0) },
    { "yellow", MKRGB(255,255,0) },
};

// Parses css-like color formats:
// #rgb, #rrggbb, rgb(r,g,b), rgba(r,g,b,a)
// rgb(r%, g%, b%), rgba(r%, g%, b%, a%)
// cf. https://developer.mozilla.org/en/CSS/color_value
static ARGB ParseCssColor(const char *color)
{
    // parse #RRGGBB and #RGB and rgb(R,G,B)
    int a, r, g, b;

    // #rgb is shorthand for #rrggbb
    if (str::Parse(color, "#%1x%1x%1x%$", &r, &g, &b)) {
        r |= (r << 4);
        g |= (g << 4);
        b |= (b << 4);
        return MKRGB(r, g, b);
    }

    if (str::Parse(color, "#%2x%2x%2x%$", &r, &g, &b) ||
        str::Parse(color, "rgb(%d,%d,%d)", &r, &g, &b)) {
        return MKRGB(r, g, b);
    }
    // parse rgba(R,G,B,A)
    if (str::Parse(color, "rgba(%d,%d,%d,%d)", &r, &g, &b, &a)) {
        return MKARGB(a, r, g, b);
    }
    // parse rgb(R%,G%,B%) and rgba(R%,G%,B%,A%)
    float fa = 1.0f, fr, fg, fb;
    if (str::Parse(color, "rgb(%f%%,%f%%,%f%%)", &fr, &fg, &fb) ||
        str::Parse(color, "rgba(%f%%,%f%%,%f%%,%f%%)", &fr, &fg, &fb, &fa)) {
        return MKARGB((int)(fa * 2.55f), (int)(fr * 2.55f), (int)(fg * 2.55f), (int)(fb * 2.55f));
    }
    // parse color names
    for (size_t i = 0; i < dimof(gKnownColors); i++) {
        if (str::EqI(gKnownColors[i].name, color)) {
            return gKnownColors[i].color;
        }
    }
    return MKARGB(0,0,0,0); // transparent
}

bool ColorData::Eq(ColorData *other) const
{
    if (type != other->type)
        return false;

    if (ColorSolid == type)
        return solid.color == other->solid.color;

    if (ColorGradientLinear == type)
    {
        return (gradientLinear.mode       == other->gradientLinear.mode) &&
               (gradientLinear.startColor == other->gradientLinear.startColor) &&
               (gradientLinear.endColor   == other->gradientLinear.endColor);
    }
    CrashIf(true);
    return false;
}

Prop::~Prop()
{
    if (PropFontName == type)
        free((void*)fontName.name);
}

bool Prop::Eq(Prop *other) const
{
    if (type != other->type)
        return false;

    switch (type) {
    case PropFontName:
        return str::Eq(fontName.name, other->fontName.name);
    case PropFontSize:
        return fontSize.size == other->fontSize.size;
    case PropFontWeight:
        return fontWeight.style == other->fontWeight.style;
    case PropPadding:
        return padding == other->padding;
    }

    if (IsColorProp(type))
        return color.Eq(&other->color);

    if (IsWidthProp(type))
        return width.width == other->width.width;

    CrashIf(true);
    return false;
}

// TODO: could speed up this at the expense of insert speed by
// sorting gAllProps by prop->type, so that we only need to
// search a subset of gAllProps. We could either explicitly
// maintain an index of PropType -> index in gAllProps of
// first property of this type or do binary search.
static Prop *FindExistingProp(Prop *prop)
{
    for (size_t i = 0; i < gAllProps->Count(); i++) {
        Prop *p = gAllProps->At(i);
        if (p->Eq(prop))
            return p;
    }
    return NULL;
}

// can't use ALLOC_BODY() because it has to create a copy of name
Prop *Prop::AllocFontName(const TCHAR *name)
{
    Prop p(PropFontName);
    p.fontName.name = name;
    Prop *newProp = FindExistingProp(&p);
    // perf trick: we didn't str::Dup() fontName.name so must NULL
    // it out so that ~Prop() doesn't try to free it
    p.fontName.name = NULL;
    if (newProp)
        return newProp;
    newProp = new Prop(PropFontName);
    newProp->fontName.name = str::Dup(name);
    gAllProps->Append(newProp);
    return newProp;
}

#define ALLOC_BODY(propType, structName, argName) \
    Prop p(propType); \
    p.structName.argName = argName; \
    Prop *newProp = FindExistingProp(&p); \
    if (newProp) \
        return newProp; \
    newProp = new Prop(propType); \
    newProp->structName.argName = argName; \
    gAllProps->Append(newProp); \
    return newProp

Prop *Prop::AllocFontSize(float size)
{
    ALLOC_BODY(PropFontSize, fontSize, size);
}

Prop *Prop::AllocFontWeight(FontStyle style)
{
    ALLOC_BODY(PropFontWeight, fontWeight, style);
}

Prop *Prop::AllocWidth(PropType type, float width)
{
    ALLOC_BODY(type, width, width);
}

Prop *Prop::AllocPadding(int top, int right, int bottom, int left)
{
    PaddingData pd = { top, right, bottom, left };
    Prop p(PropPadding);
    p.padding = pd;
    Prop *newProp = FindExistingProp(&p);
    if (newProp)
        return newProp;
    newProp = new Prop(PropPadding);
    newProp->padding = pd;
    gAllProps->Append(newProp);
    return newProp;
}

Prop *Prop::AllocColorSolid(PropType type, ARGB color)
{
    CrashIf(!IsColorProp(type));
    Prop p(type);
    p.color.type = ColorSolid;
    p.color.solid.color = color;
    Prop *newProp = FindExistingProp(&p);
    if (newProp)
        return newProp;
    newProp = new Prop(type);
    newProp->color.type = ColorSolid;
    newProp->color.solid.color = color;
    gAllProps->Append(newProp);
    return newProp;
}

Prop *Prop::AllocColorSolid(PropType type, int a, int r, int g, int b)
{
    return AllocColorSolid(type, MKARGB(a, r, g, b));
}

Prop *Prop::AllocColorSolid(PropType type, int r, int g, int b)
{
    return AllocColorSolid(type, MKARGB(0xff, r, g, b));
}

Prop *Prop::AllocColorLinearGradient(PropType type, LinearGradientMode mode, ARGB startColor, ARGB endColor)
{
    Prop p(type);
    p.color.type = ColorGradientLinear;
    p.color.gradientLinear.mode = mode;
    p.color.gradientLinear.startColor = startColor;
    p.color.gradientLinear.endColor = endColor;
    Prop *newProp = FindExistingProp(&p);
    if (newProp)
        return newProp;
    newProp = new Prop(type);
    newProp->color.type = ColorGradientLinear;
    newProp->color.gradientLinear.mode = mode;
    newProp->color.gradientLinear.startColor = startColor;
    newProp->color.gradientLinear.endColor = endColor;
    gAllProps->Append(newProp);
    return newProp;
}

Prop *Prop::AllocColorLinearGradient(PropType type, LinearGradientMode mode, const char *startColor, const char *endColor)
{
    ARGB c1 = ParseCssColor(startColor);
    ARGB c2 = ParseCssColor(endColor);
    return AllocColorLinearGradient(type, mode, c1, c2);
}

Prop *Prop::AllocColorSolid(PropType type, const char *color)
{
    ARGB col = ParseCssColor(color);
    return AllocColorSolid(type, col);
}

#undef ALLOC_BODY

// Add a property to a set, if a given PropType doesn't exist,
// replace if a given PropType already exists in the set.
void Style::Set(Prop *prop)
{
    for (size_t i = 0; i < props.Count(); i++) {
        Prop *p = props.At(i);
        if (p->type == prop->type) {
            props.At(i) = prop;
            return;
        }
    }
    props.Append(prop);
}

void Style::SetBorderWidth(float width)
{
    Set(Prop::AllocWidth(PropBorderTopWidth, width));
    Set(Prop::AllocWidth(PropBorderRightWidth, width));
    Set(Prop::AllocWidth(PropBorderBottomWidth, width));
    Set(Prop::AllocWidth(PropBorderLeftWidth, width));
}

void Style::SetBorderColor(ARGB color)
{
    Set(Prop::AllocColorSolid(PropBorderTopColor, color));
    Set(Prop::AllocColorSolid(PropBorderRightColor, color));
    Set(Prop::AllocColorSolid(PropBorderBottomColor, color));
    Set(Prop::AllocColorSolid(PropBorderLeftColor, color));
}

static bool FoundAllProps(PropToFind *props, size_t propsCount)
{
    for (size_t i = 0; i < propsCount; i++) {
        if (props[i].prop == NULL)
            return false;
    }
    return true;
}

// returns true if set, false if was already set or didn't find the prop
static bool SetPropIfFound(Prop *prop, PropToFind *propsToFind, size_t propsToFindCount)
{
    for (size_t i = 0; i < propsToFindCount; i++) {
        if (propsToFind[i].type != prop->type)
            continue;
        if (NULL == propsToFind[i].prop) {
            propsToFind[i].prop = prop;
            return true;
        }
        return false;
    }
    return false;
}

void FindProps(Style *props, PropToFind *propsToFind, size_t propsToFindCount)
{
    Prop *p;
    Style *curr = props;
    while (curr) {
        for (size_t i = 0; i < curr->props.Count(); i++) {
            p = curr->props.At(i);
            bool didSet = SetPropIfFound(p, propsToFind, propsToFindCount);
            if (didSet && FoundAllProps(propsToFind, propsToFindCount))
                return;
        }
        curr = curr->inheritsFrom;
    }
}

void FindProps(Style *first, Style *second, PropToFind *propsToFind, size_t propsToFindCount)
{
    FindProps(first, propsToFind, propsToFindCount);
    FindProps(second, propsToFind, propsToFindCount);
}

Prop *FindProp(Style *first, Style *second, PropType type)
{
    PropToFind propsToFind[1] = {
        { type, NULL }
    };
    FindProps(first, second, propsToFind, dimof(propsToFind));
    return propsToFind[0].prop;
}

// convenience function: given 2 set of properties, find font-related
// properties and construct a font object.
// Providing 2 sets of props is an optimization: conceptually it's equivalent
// to setting propsFirst->inheritsFrom = propsSecond, but this way allows
// us to have global properties for known cases and not create dummy Style just
// to link them (e.g. if VirtWndButton::styleDefault is NULL, we'll use gStyleButtonDefault)
// Caller should not delete the font - it's cached for performance and deleted at exit
// in DeleteCachedFonts()
Font *CachedFontFromProps(Style *first, Style *second)
{
    PropToFind propsToFind[3] = {
        { PropFontName, NULL },
        { PropFontSize, NULL },
        { PropFontWeight, NULL }
    };
    FindProps(first, second, propsToFind, dimof(propsToFind));
    Prop *fontName   = propsToFind[0].prop;
    Prop *fontSize   = propsToFind[1].prop;
    Prop *fontWeight = propsToFind[2].prop;
    CrashIf(!fontName || !fontSize || !fontWeight);
    FontCacheEntry c = { fontName, fontSize, fontWeight, NULL };
    for (size_t i = 0; i < gCachedFonts->Count(); i++) {
        FontCacheEntry c2 = gCachedFonts->At(i);
        if (c2.Eq(&c)) {
            CrashIf(NULL == c2.font);
            return c2.font;
        }
    }
    c.font = ::new Font(fontName->fontName.name, fontSize->fontSize.size, fontWeight->fontWeight.style);
    gCachedFonts->Append(c);
    return c.font;
}

} // namespace css
} // namespace mui