# Prospector Scanner Font Options Guide

## 🎨 ZMK/LVGL Font Reference Guide for Prospector Scanner

This document provides a comprehensive guide to available fonts for the Prospector Scanner display, including usage examples and visual characteristics.

---

## 📋 Table of Contents
1. [Currently Used Fonts](#currently-used-fonts)
2. [Available Font Families](#available-font-families)
3. [How to Enable Fonts](#how-to-enable-fonts)
4. [Font Usage Examples](#font-usage-examples)
5. [Design Recommendations](#design-recommendations)
6. [Custom Font Integration](#custom-font-integration)

---

## 🎯 Currently Used Fonts

| Widget | Current Font | Size | Purpose |
|--------|--------------|------|---------|
| Device Name | `lv_font_montserrat_20` | 20px | Large, prominent display |
| Layer Numbers | `lv_font_montserrat_20` | 20px | Clear visibility |
| Layer Title | `lv_font_montserrat_12` | 12px | Subtle label |
| Connection Status | `lv_font_montserrat_12` | 12px | Compact info |
| Battery Text | `lv_font_montserrat_12` | 12px | Space-efficient |

---

## 🎨 Available Font Families

### 1. **Montserrat Family** (Modern Sans-Serif)

**Characteristics**: Clean, modern, excellent readability, professional appearance

#### Standard Sizes
```c
// Small sizes (8-14px) - Good for labels and small text
&lv_font_montserrat_8    // Tiny labels
&lv_font_montserrat_10   // Very small text
&lv_font_montserrat_12   // Small labels (currently used)
&lv_font_montserrat_14   // Slightly larger labels

// Medium sizes (16-24px) - Good for main content
&lv_font_montserrat_16   // Standard text
&lv_font_montserrat_18   // Comfortable reading
&lv_font_montserrat_20   // Prominent text (currently used)
&lv_font_montserrat_22   // Large clear text
&lv_font_montserrat_24   // Extra large text

// Large sizes (26-36px) - Good for headers
&lv_font_montserrat_26   // Small headers
&lv_font_montserrat_28   // Medium headers
&lv_font_montserrat_30   // Large headers
&lv_font_montserrat_32   // Extra large headers
&lv_font_montserrat_34   // Huge headers
&lv_font_montserrat_36   // Giant headers

// Extra large sizes (38-48px) - Good for single numbers/icons
&lv_font_montserrat_38   // Display size
&lv_font_montserrat_40   // Large display
&lv_font_montserrat_42   // Extra large display
&lv_font_montserrat_44   // Huge display
&lv_font_montserrat_46   // Giant display
&lv_font_montserrat_48   // Maximum size
```

#### Style Variants (Must be enabled separately)
```c
// Bold variants - Add emphasis
&lv_font_montserrat_12_bold
&lv_font_montserrat_16_bold
&lv_font_montserrat_20_bold
&lv_font_montserrat_24_bold
&lv_font_montserrat_28_bold
&lv_font_montserrat_32_bold

// Italic variants - Add elegance
&lv_font_montserrat_12_italic
&lv_font_montserrat_16_italic
&lv_font_montserrat_20_italic
&lv_font_montserrat_24_italic
```

### 2. **Unscii Family** (Retro Pixel Font)

**Characteristics**: 8-bit aesthetic, nostalgic feel, perfect squares, gaming style

```c
&lv_font_unscii_8    // Classic 8x8 pixel font
&lv_font_unscii_16   // Larger 16x16 pixel font
```

**Best for**: 
- Retro/gaming themed displays
- Status indicators
- Creating nostalgic atmosphere
- High contrast requirements

### 3. **DejaVu Sans** (Extended Character Support)

**Characteristics**: Wide language support, readable, traditional

```c
&lv_font_dejavu_16_persian_hebrew  // Middle Eastern language support
```

**Best for**:
- International character support
- Mixed language displays
- Special symbols

### 4. **Simsun** (CJK Support)

**Characteristics**: Chinese/Japanese/Korean character support

```c
&lv_font_simsun_16_cjk  // Full CJK character set
```

**Best for**:
- Asian language support
- Mixed English/CJK text

---

## 🛠️ How to Enable Fonts

### Step 1: Edit Configuration File

Add to `/home/ogu/workspace/prospector/prospector-zmk-module/boards/shields/prospector_scanner/prospector_scanner.conf`:

```conf
# Enable additional Montserrat sizes
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_MONTSERRAT_32=y

# Enable bold variants
CONFIG_LV_FONT_MONTSERRAT_20_BOLD=y
CONFIG_LV_FONT_MONTSERRAT_24_BOLD=y
CONFIG_LV_FONT_MONTSERRAT_28_BOLD=y

# Enable italic variants
CONFIG_LV_FONT_MONTSERRAT_20_ITALIC=y
CONFIG_LV_FONT_MONTSERRAT_24_ITALIC=y

# Enable retro fonts
CONFIG_LV_FONT_UNSCII_8=y
CONFIG_LV_FONT_UNSCII_16=y

# Enable international support
CONFIG_LV_FONT_DEJAVU_16_PERSIAN_HEBREW=y
CONFIG_LV_FONT_SIMSUN_16_CJK=y
```

### Step 2: Use in Code

```c
// Example: Change layer number font to larger bold
lv_obj_set_style_text_font(widget->layer_labels[i], &lv_font_montserrat_24_bold, 0);

// Example: Use retro font for special effect
lv_obj_set_style_text_font(retro_label, &lv_font_unscii_16, 0);
```

---

## 💡 Font Usage Examples

### Example 1: Enhanced Layer Display
```c
// Layer title - small and subtle
lv_obj_set_style_text_font(layer_title, &lv_font_montserrat_12, 0);

// Layer numbers - large and bold for visibility
lv_obj_set_style_text_font(layer_number, &lv_font_montserrat_28_bold, 0);
```

### Example 2: Modern Minimalist Design
```c
// All text in consistent size
lv_obj_set_style_text_font(all_labels, &lv_font_montserrat_20, 0);

// But use bold for active elements
lv_obj_set_style_text_font(active_element, &lv_font_montserrat_20_bold, 0);
```

### Example 3: Mixed Retro Style
```c
// Numbers in pixel font
lv_obj_set_style_text_font(numbers, &lv_font_unscii_16, 0);

// Text in modern font
lv_obj_set_style_text_font(text, &lv_font_montserrat_16, 0);
```

---

## 🎨 Design Recommendations

### 1. **Elegant Professional** ✨
- **Primary**: `montserrat_24` or `montserrat_28`
- **Secondary**: `montserrat_16`
- **Small text**: `montserrat_12`
- **Emphasis**: Use bold variants

### 2. **Gaming/Retro** 🕹️
- **Numbers**: `unscii_16`
- **Primary text**: `montserrat_20`
- **Small text**: `unscii_8`

### 3. **High Visibility** 👁️
- **All text**: `montserrat_bold` variants
- **Large elements**: `montserrat_32_bold` or larger
- **High contrast colors**

### 4. **Space Efficient** 📏
- **Primary**: `montserrat_16`
- **Secondary**: `montserrat_12`
- **Minimize padding**

---

## 🔧 Custom Font Integration (Advanced)

### Step 1: Prepare Font File
1. Download `.ttf` or `.otf` font file
2. Popular sources:
   - Google Fonts
   - Adobe Fonts
   - Open source font repositories

### Step 2: Convert to LVGL Format
1. Use [LVGL Font Converter](https://lvgl.io/tools/fontconverter)
2. Settings:
   - Select font file
   - Choose size (px)
   - Select character range
   - Set bits per pixel (usually 4)
   - Output format: C array

### Step 3: Integrate into ZMK
1. Add generated `.c` file to project
2. Include in CMakeLists.txt
3. Register font in code
4. Use like built-in fonts

### Popular Custom Font Suggestions
- **Inter**: Modern UI font, excellent readability
- **Roboto**: Google's Material Design font
- **Fira Code**: Programmer font with ligatures
- **IBM Plex**: Professional and versatile
- **Source Sans Pro**: Adobe's UI font

---

## 📊 Font Size Reference Chart

| Use Case | Recommended Size | Font Options |
|----------|------------------|--------------|
| Tiny Labels | 8-10px | `montserrat_8`, `unscii_8` |
| Small Text | 12-14px | `montserrat_12`, `montserrat_14` |
| Normal Text | 16-20px | `montserrat_16`, `montserrat_18`, `montserrat_20` |
| Headers | 24-32px | `montserrat_24`, `montserrat_28`, `montserrat_32` |
| Display | 36-48px | `montserrat_36`, `montserrat_40`, `montserrat_48` |

---

## 🎯 Quick Decision Guide

**Q: Want more elegant look?**
→ Use larger Montserrat sizes with bold variants

**Q: Want retro/gaming aesthetic?**
→ Mix Unscii pixel fonts with Montserrat

**Q: Need maximum readability?**
→ Use Montserrat bold variants in larger sizes

**Q: Limited screen space?**
→ Stick with current sizes but optimize spacing

**Q: Want unique personality?**
→ Consider custom font integration

---

## 📝 Notes

- Font availability depends on firmware size constraints
- Each enabled font increases firmware size
- Test on actual hardware for best results
- Consider battery impact of rendering complex fonts
- Some fonts may require additional configuration flags

---

**Document Version**: 1.0  
**Last Updated**: 2025-01-28  
**Compatible with**: ZMK with LVGL display support