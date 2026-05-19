# Ghost — Visual Design System

## Core Design Direction

The poster uses a minimal cyber-noir aesthetic with:

- ultra-dark backgrounds
- subtle purple/blue neon accents
- soft volumetric glow
- sparse UI-inspired geometry
- futuristic emptiness
- non-aggressive "digital haunting"

The vibe is:

- terminal + atmospheric sci-fi + abstract ghost presence

Not horror. More:

- mysterious
- intelligent
- stealthy
- futuristic
- elegant

---

## 1. Color System

### Primary Background Palette

**Absolute Black**
`--bg-0: #02040A;`

Used for:
- page background
- hero sections
- fullscreen overlays

**Deep Navy**
`--bg-1: #070B16;`

Used for:
- gradients
- cards
- navbars

**Midnight Indigo**
`--bg-2: #0C1024;`

Used for:
- elevated surfaces
- section separation

### Accent Palette

**Neon Violet**
`--accent-violet: #8B5CF6;`

Used sparingly for:
- borders
- highlights
- hover glows
- focus rings

**Electric Purple**
`--accent-purple: #A855F7;`

Used for:
- CTA glow
- animated particles
- active states

**Soft Cyber Blue**
`--accent-blue: #60A5FA;`

Used for:
- UI lines
- secondary highlights
- code-like decorations

**Glow White**
`--glow-white: #E5F0FF;`

Used for:
- typography glow
- ghost cloud highlights
- subtle illumination

### Text Colors

**Primary Text**
`--text-primary: #F5F7FF;`

**Secondary Text**
`--text-secondary: #94A3B8;`

**Muted Text**
`--text-muted: #64748B;`

---

## 2. Typography System

### Font Style

Use:
- futuristic sans-serif
- geometric spacing
- light weights

Recommended fonts:

- **Best Match:** Space Grotesk
- Sora
- General Sans
- Neue Montreal
- Inter Tight

### Heading Style

**Hero Heading**
```css
font-size: clamp(4rem, 10vw, 8rem);
font-weight: 500;
letter-spacing: -0.04em;
line-height: 0.9;
```

Characteristics:
- huge
- compressed vertically
- cinematic

### Metadata Typography

The "1 june 2026" text style:

```css
font-size: 1.4rem;
font-weight: 300;
letter-spacing: 0.45em;
text-transform: lowercase;
```

Important:
- lowercase text
- huge tracking
- thin font weight

---

## 3. Layout Philosophy

### Massive Negative Space

The poster is:
- 75% empty darkness
- 25% visual content

This is important.

Avoid:
- clutter
- multiple cards
- dense sections

### Vertical Composition

Structure:

```
[ EMPTY SPACE ]

      Ghost Cloud

[ EMPTY SPACE ]

      Date

[ GRID FLOOR ]
```

This creates:
- cinematic scale
- loneliness
- futuristic depth

---

## 4. Background System

### Multi-Layer Background

**Layer 1 — Base Gradient**
```css
background:
radial-gradient(circle at top,
#111827 0%,
#02040A 60%);
```

**Layer 2 — Vignette**
Dark edges:

```css
box-shadow:
inset 0 0 200px rgba(0,0,0,0.9);
```

**Layer 3 — Digital Noise**
Use:
- 1–2% opacity grain
- animated subtle noise texture

This prevents flatness.

**Layer 4 — Grid Floor**
Perspective grid:
- ultra subtle
- thin neon lines
- fades into darkness

Opacity: `0.08 - 0.14`

---

## 5. The Ghost Element

### Design Language

The "ghost" should NOT be:
- a cartoon ghost
- a horror face
- realistic apparition

Instead:
- cloud intelligence
- abstract entity
- atmospheric form

### Shape Characteristics

The cloud form:
- soft edges
- blurred silhouette
- floating
- semi-symmetrical

Think: AI spirit hidden in clouds

### Eye Design

Eyes are:
- minimal slits
- soft glow
- rectangular arches

Never:
- realistic eyes
- pupils
- horror sockets

Glow:
```css
filter: blur(6px);
```

---

## 6. UI Ornamentation

### Minimal Interface Decorations

Use:
- thin lines
- tiny squares
- tracking dots
- glitch fragments

Inspired by:
- terminal UIs
- surveillance overlays
- cyberpunk HUDs

### Rules

Keep Everything Sparse

Good:
- 3 floating lines

Bad:
- full cyberpunk dashboard

### Line Styling

```css
border-color: rgba(139,92,246,0.18);
```

Width: `1px`

---

## 7. Motion System

### Motion Style

Everything moves:
- slowly
- smoothly
- almost imperceptibly

### Animations

**Floating Cloud**
```css
transform: translateY(-6px);
```
Duration: `8s ease-in-out infinite`

**Ambient Glow Pulse**
```css
opacity: 0.7 → 1 → 0.7
```
Duration: `4–6s`

**Grid Drift**
Very subtle horizontal movement:
```css
translateX(2px)
```

---

## 8. Blur + Glow System

### Essential Principle

The poster relies heavily on:
- atmospheric blur
- bloom lighting
- soft edges

### Glow Values

**Violet Glow**
```css
box-shadow: 0 0 30px rgba(168,85,247,0.35);
```

**Blue Glow**
```css
box-shadow: 0 0 25px rgba(96,165,250,0.25);
```

### Blur Scale

Use:
```css
filter: blur(40px);
```

for:
- background clouds
- volumetric fog
- ghost softness

---

## 9. Component Design Rules

### Buttons

Style:
- transparent
- outlined
- subtle glow

Example:

```css
background: rgba(255,255,255,0.02);
border: 1px solid rgba(139,92,246,0.2);
backdrop-filter: blur(20px);
```

Hover:

```css
border-color: #A855F7;
box-shadow: 0 0 20px rgba(168,85,247,0.2);
```

### Cards

Cards should:
- almost disappear into background
- use glassmorphism lightly

Avoid:
- bright cards
- strong borders

---

## 10. Website Implementation Style

### Ideal Stack

Frontend:
- Next.js
- Tailwind
- Framer Motion

Effects:
- CSS gradients
- SVG overlays
- canvas particles
- noise texture

---

## 11. Tailwind Theme Example

```js
colors: {
  background: {
    DEFAULT: "#02040A",
    elevated: "#0C1024"
  },
  accent: {
    violet: "#8B5CF6",
    purple: "#A855F7",
    blue: "#60A5FA"
  }
}
```

---

## 12. Hero Section Structure

```
Fullscreen Container
 ├── Gradient Background
 ├── Noise Layer
 ├── Grid Perspective
 ├── Floating Ghost Cloud
 ├── Sparse UI Lines
 └── Metadata Text
```

---

## 13. Atmosphere Keywords

Use these as creative constraints:

- stealth
- spectral
- terminal
- atmospheric
- cinematic
- futuristic
- silent
- neon minimal
- digital void
- intelligent darkness
- abstract AI entity

Avoid:

- gamer UI
- neon overload
- horror tropes
- bright cyberpunk cities
- excessive purple
- skulls/blood
- aggressive glitching

---

## 14. Design Inspirations

Closest visual inspirations:

- Blade Runner 2049 minimal scenes
- Cyberpunk 2077 UI overlays
- Ghost in the Shell atmospheric tech mood
- Control emptiness + mystery
- Tron: Legacy glow language
- sci-fi terminal interfaces
- stealth hacker aesthetics
