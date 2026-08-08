// <!-- Version 4 -->
/* ============================================
   JCORP NOMAD - THEME MANAGER WITH SD CARD PERSISTENCE
   Version: 4.0
   Last Updated: February 22, 2026
   Description: SD card-based theme storage (persists across devices & reboots)
   ============================================ */

const ThemeManager = {
  STORAGE_KEY: 'nomad_dark_mode',
  THEME_STORAGE_KEY: 'nomad_custom_theme',
  THEME_FILE_PATH: '/.system-theme.json',
  
  init() {
    try {
      const isDark = this.getDarkMode();
      this.applyDarkMode(isDark);
      
      // Load and apply custom theme from SD card (fallback to localStorage for compatibility)
      this.loadThemeFromServer().then(savedTheme => {
        if (savedTheme) {
          this.applyThemeColors(savedTheme);
          console.log('[ThemeManager] Loaded theme from SD card');
        } else {
          // Fallback to localStorage if SD card theme doesn't exist yet
          const localTheme = this.loadThemeFromStorage();
          if (localTheme) {
            this.applyThemeColors(localTheme);
            console.log('[ThemeManager] Loaded theme from localStorage (fallback)');
          }
        }
      });
      
      console.log('[ThemeManager] Initialized with SD card persistence');
    } catch (e) {
      console.error('[ThemeManager] Init failed:', e);
    }
  },

  getDarkMode() {
    try {
      return localStorage.getItem(this.STORAGE_KEY) === 'true';
    } catch (e) {
      return false;
    }
  },

  setDarkMode(isDark) {
    try {
      localStorage.setItem(this.STORAGE_KEY, isDark.toString());
      this.applyDarkMode(isDark);
    } catch (e) {
      console.error('[ThemeManager] Could not set dark mode:', e);
    }
  },

  applyDarkMode(isDark) {
    if (isDark) {
      document.body.classList.add('dark');
    } else {
      document.body.classList.remove('dark');
    }
  },

  toggleDarkMode() {
    const current = this.getDarkMode();
    this.setDarkMode(!current);
  },

  async saveThemeToServer(colors) {
    console.log('[ThemeManager] Saving theme to SD card...');
    console.log('[ThemeManager] Colors:', colors);
    
    try {
      // Delete existing theme file using POST to /delete endpoint with FormData
      try {
        const deleteFormData = new FormData();
        deleteFormData.append('filename', this.THEME_FILE_PATH);
        
        const deleteResponse = await fetch('/delete', {
          method: 'POST',
          body: deleteFormData
        });
        
        if (deleteResponse.ok) {
          console.log('[ThemeManager] Existing theme file deleted successfully');
        } else {
          console.log('[ThemeManager] No existing theme file to delete (404 is OK)');
        }
      } catch (e) {
        console.log('[ThemeManager] Delete request failed (file may not exist):', e);
      }
      
      // Wait for delete to complete
      await new Promise(resolve => setTimeout(resolve, 500));
      
      // Create JSON content for the theme file
      const themeData = JSON.stringify(colors, null, 2);
      const blob = new Blob([themeData], { type: 'application/json' });
      const file = new File([blob], '.system-theme.json', { type: 'application/json' });
      
      // Upload to SD card root
      const formData = new FormData();
      formData.append('dir', '');
      formData.append('file', file, '.system-theme.json');
      
      console.log('[ThemeManager] Uploading theme file to SD card...');
      const response = await fetch('/upload', {
        method: 'POST',
        body: formData
      });
      
      if (response.ok) {
        localStorage.setItem(this.THEME_STORAGE_KEY, JSON.stringify(colors));
        this.applyThemeColors(colors);
        console.log('[ThemeManager] Theme saved to SD card successfully!');
        return { success: true, message: 'Theme saved to SD card! All devices will use this theme.' };
      } else {
        const errorText = await response.text();
        throw new Error(`Upload failed: ${response.status} - ${errorText}`);
      }
    } catch (error) {
      console.error('[ThemeManager] SD card save error:', error);
      try {
        localStorage.setItem(this.THEME_STORAGE_KEY, JSON.stringify(colors));
        this.applyThemeColors(colors);
        return { success: true, message: 'Theme saved to browser only (SD card save failed).' };
      } catch (e) {
        return { success: false, message: error.message };
      }
    }
  },

  async loadThemeFromServer() {
    try {
      const response = await fetch(this.THEME_FILE_PATH + '?t=' + Date.now());
      if (!response.ok) {
        return null;
      }
      const theme = await response.json();
      console.log('[ThemeManager] Loaded theme from SD card');
      return theme;
    } catch (e) {
      console.log('[ThemeManager] No theme file on SD card yet');
      return null;
    }
  },

  saveThemeToStorage(colors) {
    return this.saveThemeToServer(colors);
  },

  loadThemeFromStorage() {
    try {
      const stored = localStorage.getItem(this.THEME_STORAGE_KEY);
      if (stored) {
        return JSON.parse(stored);
      }
    } catch (e) {
      console.error('[ThemeManager] Failed to load theme from localStorage:', e);
    }
    return null;
  },

  applyThemeColors(colors) {
    console.log('[ThemeManager] Applying theme colors...');
    
    let styleEl = document.getElementById('custom-theme-style');
    if (!styleEl) {
      styleEl = document.createElement('style');
      styleEl.id = 'custom-theme-style';
      document.head.appendChild(styleEl);
    }
    
    const css = `
      :root {
        --primary: ${colors.primary} !important;
        --primary-dark: ${colors.primaryDark} !important;
        --bg: ${colors.bgLight} !important;
        --card: ${colors.cardLight} !important;
        --card-bg: ${colors.cardLight} !important;
        --text: ${colors.textLight} !important;
        --muted: ${colors.mutedLight} !important;
      }
      
      body.dark {
        --bg: ${colors.bgDark} !important;
        --card: ${colors.cardDark} !important;
        --card-bg: ${colors.cardDark} !important;
        --text: ${colors.textDark} !important;
        --muted: ${colors.mutedDark} !important;
        --primary: ${colors.primaryDarkTheme} !important;
      }
    `;
    
    styleEl.textContent = css;
    console.log('[ThemeManager] Theme colors applied!');
  },

  async resetTheme() {
    console.log('[ThemeManager] Resetting theme...');
    try {
      localStorage.removeItem(this.THEME_STORAGE_KEY);
      const styleEl = document.getElementById('custom-theme-style');
      if (styleEl) {
        styleEl.remove();
      }
      
      // Try to delete theme file from SD card
      try {
        const deleteFormData = new FormData();
        deleteFormData.append('filename', this.THEME_FILE_PATH);
        await fetch('/delete', { method: 'POST', body: deleteFormData });
      } catch (e) {
        // Ignore errors
      }
      
      console.log('[ThemeManager] Theme reset to default!');
      return { success: true, message: 'Theme reset to default!' };
    } catch (error) {
      console.error('[ThemeManager] Reset error:', error);
      return { success: false, message: error.message };
    }
  },

  async loadThemes() {
    console.log('[ThemeManager] Loading themes from /default-themes.json');
    try {
      const response = await fetch('/default-themes.json');
      if (!response.ok) {
        console.error('[ThemeManager] Failed to load themes, status:', response.status);
        throw new Error('Failed to load themes');
      }
      const data = await response.json();
      console.log('[ThemeManager] Loaded themes:', data.themes?.length || 0);
      return data.themes || [];
    } catch (error) {
      console.error('[ThemeManager] Load themes error:', error);
      return [];
    }
  }
};

if (typeof window !== 'undefined') {
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => ThemeManager.init());
  } else {
    ThemeManager.init();
  }
}
