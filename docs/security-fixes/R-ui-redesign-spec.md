# {{APP_NAME}} — UI Redesign Specification

> **Read [README.md](README.md) first.** Its global power rules apply here too. The section "R: how the UI redesign fits the pack" covers ordering, how this spec meets the security workstreams, and the questions the owner has been asked. Added 2026-11-26 at the owner's request, as the owner supplied it. Amended the same day with the owner's answers to R-Q1 (branding), R-Q2 (theme scope) and R-Q3 (home cards); the amended passages are marked *(amended)*. Otherwise only markdown formatting was added. The visual reference ("the supplied modern UI mockup") is [assets/nomad-ui-mockup.png](assets/nomad-ui-mockup.png). The name and tree logo shown in the mockup are placeholders for layout only.
>
> **`{{APP_NAME}}` is a placeholder** for the fork's own product name, which the owner will supply. Until then, build with the name held in one place (see *Branding scope*) so that filling it in is a one-line change.

## 1. Project overview

Redesign the existing Jcorp Nomad personal media library into a modern, polished, responsive web application.

The application runs on an ESP32, so the design must be lightweight, efficient, and suitable for a small embedded device. The goal is to achieve a premium, modern interface without introducing unnecessary rendering complexity, large dependencies, or excessive resource usage.

The redesign must preserve the existing functionality and navigation while making the application feel like a cohesive, modern product.

### Product identity

- Application name: {{APP_NAME}} *(amended: the fork's own name, supplied by the owner)*
- Tagline: Your personal media library
- Brand mark: A new logo supplied by the owner, replacing the Jcorp Nomad tree logo. *(amended)*
- Default theme: Modern blue.
- Design direction: Minimal, clean, soft, and contemporary.
- Primary objective: A beautiful interface that remains fast and practical on an ESP32.

Use {{APP_NAME}} consistently and introduce no other product name. Replace all visible instances of “Jcorp Nomad”, and of “Nomad” used as the product name, with “{{APP_NAME}}”. *(amended)*

### Branding scope *(amended)*

The original project's `LICENSING.md` says the “Jcorp” and “Nomad” names and the Jcorp and Nomad logos are Jcorp Tech's trademarks. A shared, changed version must use its own name and replace the logo files. So “visible” means everywhere a person sees the product's name or logo:

- Every page's header, `<title>`, and visible text.
- The logo and icon files: `SD_Card_Template/Logo.png`, `favicon.ico`, and any touch or app icons.
- The device's own screen: the boot image `firmware/JcorpNomadProject/ui_img_1859342711.c`, and the main and status screens' text and logo.
- The default hotspot name (`Jcorp_Nomad`; see 6.1 for the per-board suffix).
- The DLNA/UPnP friendly name, manufacturer and SSDP server string (`NomadDLNA.cpp`).
- The OPDS feed title, server-built pages (`NomadPageBody.h`), firmware messages shown in the admin panel, and the README.

Hold the name in one place for the web interface (one constant read by every page) and one place for the firmware (one macro), and keep the logo as one set of files. Then renaming is a one-line change plus new image files.

Internal identifiers stay as they are: file names, `Nomad*` sources, `NOMAD_*` macros and `nomad_*` storage keys. People don't see them, and renaming them would break upgrades and upstream merges.

## 2. Design direction

Use the supplied modern UI mockup as the visual reference.

The new interface should feature:

- A clean, consistent design system across all application pages.
- Soft, light backgrounds with subtle contrast between content areas.
- Rounded cards and controls.
- A refined blue navigation header.
- Generous spacing without wasting screen space.
- Clear typography and strong visual hierarchy.
- Subtle shadows and borders rather than heavy visual effects.
- Consistent iconography.
- A responsive layout that works on desktop, mobile, and embedded displays.

The application should feel like a modern native application rather than a traditional website.

Avoid excessive gradients, glassmorphism, large decorative illustrations, unnecessary animation, or visual effects that increase rendering cost.

## 3. Global application layout

### Header

Create a consistent header component used throughout the application.

The header should contain:

- {{APP_NAME}} logo. *(amended)*
- Application name, “{{APP_NAME}}”. *(amended)*
- Back-to-menu or navigation control when inside a section.
- Page title where appropriate.
- Theme toggle.
- Settings access.
- Contextual controls relevant to the current page.

The header should use the active theme’s primary colour.

On smaller screens, reduce the header to the essential navigation and action icons.

### Home screen

The home screen should closely follow the supplied mockup.

Layout:

1. Branded header containing the logo, {{APP_NAME}}, theme toggle, and settings. *(amended)*
2. Welcome heading and tagline.
3. Prominent search field.
4. Main navigation cards.
5. Optional connection or device status indicator.

The navigation cards should provide access to:

- Movies
- Shows
- Music
- Books
- Gallery
- Files
- Games
- Maps
- Workshop

*(amended)* A card is shown only if the admin has that section switched on (the per-page switches in `/.system-ui.json`). The other sections (Archive, Translate, Cookbook and Chat) get no card of their own. When at least one of them is switched on, a final **More** card leads to them.

The cards should use simple icons, readable labels, and consistent spacing.

On desktop, use a grid layout where appropriate. On mobile, use a single-column layout.

The home screen must remain usable on a small display without requiring excessive scrolling.

## 4. Colour palette customization

This is a core feature of the redesign.

Users must be able to change the application’s colour palette without reloading the application or losing their current location.

### Theme settings

Add a dedicated Theme section inside Settings. *(amended)* This Settings panel is open to every visitor, with no sign-in. The admin-only actions below appear only to a signed-in admin.

Provide three appearance modes:

- Light
- Dark
- Match device

The user should also be able to choose from several predefined colour palettes.

Include the following initial palettes:

| Palette | Primary | Background | Character |
|---|---|---|---|
| {{APP_NAME}} Blue | #0878F9 | #F4F5FA | Clean, modern blue |
| Forest | #16865B | #F2F7F4 | Natural green |
| Lavender | #7557D9 | #F6F3FC | Soft purple |
| Sunset | #E65A68 | #FFF5F3 | Warm coral |
| Ocean | #087FAD | #F0F7FA | Deep blue |
| Midnight | #6366F1 | #151923 | Dark indigo |

Each palette should have a small preview showing its main colours.

Selecting a palette must update the entire application immediately.

*(amended)* These six are the main list. The existing 28 built-in themes (`default-themes.json`) stay available under **More palettes**, loaded only when that list is opened.

### Card default and personal choice *(amended)*

Nomad already has one theme for every device: the admin sets it, and it's stored on the card in `/.system-theme.json` (`theme-manager.js`). Keep that as **everyone's starting point**:

- A visitor with no choice of their own sees the card's theme and mode.
- A visitor's own choice of mode, palette and custom colours overrides the card's theme in their browser only. A **Use the default** button drops the visitor's choice and returns to the card's theme.
- A signed-in admin also sees **Save as everyone's default**, which writes the current palette and mode to the card through the existing mechanism. This covers what the old Theme Customizer page (`theme-customization-ui.html`) does. Keep that page until the new editor fully replaces it, then retire it.

### Custom palette editor

Allow users to create their own colour palette.

Provide controls for:

- Primary colour
- Accent colour
- Background colour
- Surface/card colour
- Text colour

Use a native colour input where supported.

Include a reset button to restore the selected palette’s original values.

The application must remain readable when custom colours are selected. Automatically determine suitable text contrast for primary-colour buttons and other prominent elements.

### Theme persistence

The selected appearance mode, palette, and custom colours should persist between page navigation and application restarts. *(amended)* A visitor's own choice persists in their browser; the card-wide default persists on the card, as it does today.

Use the simplest persistence mechanism supported by the existing application.

Prefer browser local storage when available. If the application already has a configuration or settings storage mechanism, reuse it.

Do not introduce a database or additional backend service solely for theme persistence.

### Implementation requirements

Use a small set of CSS custom properties as the theme system.

Example:

```css
:root {
  --color-primary: #0878F9;
  --color-accent: #49A5FF;
  --color-background: #F4F5FA;
  --color-surface: #FFFFFF;
  --color-text: #1C2738;
  --color-text-muted: #697386;
  --color-border: #E2E6EF;
  --color-success: #12A875;
  --radius-card: 16px;
  --radius-control: 12px;
}
```

All application components must reference these variables rather than hardcoded colours.

Do not create separate, duplicated stylesheets for each palette.

## 5. Component design

Create a small, reusable set of UI components.

### Buttons

- Rounded corners.
- Consistent height and padding.
- Clear primary, secondary, and icon-only variants.
- Visible hover, focus, active, and disabled states.
- Minimum touch target of approximately 44 × 44 CSS pixels where practical.

### Cards

- Surface colour from the active theme.
- Subtle border or shadow.
- Consistent corner radius.
- Consistent internal spacing.
- Clear separation between content and actions.

### Search fields

- Full-width on mobile.
- Rounded corners.
- Integrated search icon.
- Clear placeholder text.
- No excessive borders or decorative effects.

### Icons

Use a consistent, lightweight icon set or the existing application’s icons.

Avoid introducing a large icon library if the current application does not already use one.

### Typography

Use a system font stack to avoid downloading external fonts.

Use a consistent scale for:

- Page titles
- Section headings
- Card titles
- Body text
- Secondary text
- Metadata

Ensure text remains readable on small screens.

## 6. Existing application sections

The redesign must preserve all existing functionality.

Do not remove features, replace working functionality with static mockups, or change the underlying data structure without a clear requirement.

### Media library

Preserve the existing Movies, Shows, Music, Books, Gallery, and Files sections.

Apply the new visual system to each section, including:

- Search and filtering.
- Sorting.
- List and grid views where supported.
- Media thumbnails and cover art.
- Item selection and detail views.
- Playback, viewing, and downloading actions.

### Books and PDF viewer

Preserve the existing document viewer functionality.

The viewer should use a minimal toolbar with readable controls, appropriate page navigation, and a clear reading area.

Do not introduce additional rendering effects or change the underlying document rendering engine.

### Music

Preserve the existing music player and library functionality.

Use the new theme for playlists, track lists, playback controls, and the player interface.

Keep the player compact and responsive.

### Gallery

Preserve image browsing, thumbnails, sorting, and existing upload functionality.

Use a clean, consistent thumbnail grid.

Do not load full-resolution images into the gallery grid when thumbnails are available.

### Files

Preserve file listing, sorting, filtering, and downloading.

Use clear file-type icons and compact metadata.

### Maps, games, recipes, chat, and workshop

Retain the existing functionality shown in the supplied screenshots.

These sections should use the same header, typography, controls, colour system, and spacing as the rest of the application.

Do not attempt to redesign their underlying functionality as part of this task.

## 7. ESP32 performance requirements

The application must remain lightweight enough to run comfortably on the existing ESP32 hardware.

Visual quality must come from layout, spacing, colour, typography, and carefully chosen assets rather than expensive effects.

### Required constraints

- Reuse the existing framework and build system.
- Do not introduce a new frontend framework.
- Do not add large UI libraries.
- Do not add large animation libraries.
- Avoid unnecessary JavaScript dependencies.
- Avoid external font downloads.
- Avoid large background images and decorative assets.
- Avoid continuous animations and unnecessary timers.
- Avoid expensive blur, backdrop-filter, and complex shadow effects.
- Avoid unnecessary DOM updates and full-page rerenders.
- Avoid loading large images or media until required.
- Keep theme switching lightweight and immediate.

Prefer CSS transitions for simple interactions.

Use existing application functionality and components wherever possible.

### Resource considerations

The ESP32 may have limited RAM, flash, and CPU resources.

The redesign must not significantly increase the application’s memory usage, bundle size, or initial loading time.

If the existing application uses a lightweight server-rendered or static HTML approach, preserve that architecture.

Do not add a large client-side application framework solely to implement the new design.

## 8. Responsive behaviour

The application should adapt to different screen sizes.

### Mobile

- Single-column navigation.
- Full-width search.
- Compact header.
- Comfortable touch targets.
- Minimal horizontal scrolling.

### Tablet

- Two-column card layouts where appropriate.
- Balanced spacing.
- Compact navigation.

### Desktop

- Multi-column layouts where appropriate.
- Use available screen space efficiently.
- Maintain readable content widths.

The application should remain functional on narrow displays and older embedded browsers.

## 9. Accessibility and usability

- Maintain readable text contrast.
- Provide visible keyboard focus states.
- Use semantic HTML where possible.
- Ensure buttons and controls have meaningful labels.
- Do not rely on colour alone to communicate status.
- Respect reduced-motion preferences where supported.
- Ensure the application remains usable without hover interactions.

## 10. Implementation approach

Before making changes:

1. Inspect the existing project structure.
2. Identify the current framework, styling approach, routing, and state management.
3. Identify the existing theme and settings mechanisms.
4. Identify the current ESP32 serving method and resource constraints.
5. Identify existing functionality that must remain unchanged.

Then implement the redesign in stages:

1. Establish the {{APP_NAME}} design system and CSS variables. *(amended)*
2. Redesign the global header and navigation.
3. Redesign the home screen.
4. Implement palette selection and custom colour settings.
5. Apply the new components and theme to existing sections.
6. Verify responsive behaviour and performance.
7. Test the application on the actual ESP32 where possible.

Do not rewrite the entire application unless the existing architecture makes that unavoidable.

## 11. Acceptance criteria

The implementation is complete when:

- The application is branded as {{APP_NAME}} throughout, including the device screen, hotspot name and TV (DLNA) name, with the owner's logo replacing the Jcorp/Nomad logo files. *(amended)*
- The home screen matches the supplied modern design direction.
- All existing sections and functionality remain accessible.
- Users can switch between light and dark appearance modes.
- Users can select predefined colour palettes.
- Users can create and save custom palettes.
- Each visitor's choice overrides the admin's card-wide default, which remains every new visitor's starting point. *(amended)*
- Theme changes apply consistently across all sections.
- Theme settings persist between sessions.
- The interface works on mobile, tablet, and desktop.
- The application remains responsive on the ESP32.
- No unnecessary heavy dependencies have been introduced.
- Existing media, file, and document functionality continues to work.

## Final design principle

Make {{APP_NAME}} feel like a modern, premium personal media library through simplicity and consistency.

The application should look beautiful because the design is well considered, not because the device is working harder to render it.

Preserve the existing functionality, keep the implementation lightweight, and make the colour system flexible enough that the user can give {{APP_NAME}} their own identity.
