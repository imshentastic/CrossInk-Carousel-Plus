// CrossInk / CrumBLE EPUB optimizer module.
// Lazy-loaded by the File Manager page (FilesPage.html) via ensureOptimizer()
// only when the user engages EPUB optimization. Keeping this ~heavy code out of
// the base page lets the File Manager page serve in one small chunk, so browsing
// files over a flaky WiFi link no longer craters the device's tiny heap.
//
// All shared module-level state (DEVICE_PROFILES, JPEG_QUALITY, imageStates,
// logSection, DEFENSIVE_STYLE, etc.) and the small settings UI handlers remain
// declared in FilesPage.html's inline script, which always runs first; the
// functions below reference them through the shared global scope.

  // ============================================================================
  // Image Picker Functions
  // ============================================================================

  /**
   * Extract images from EPUB for preview
   * Returns array of {path, name, dataUrl, width, height}
   */
  async function extractImagesForPreview(file) {
    const zip = await JSZip.loadAsync(file);
    const imageExtensions = ['.png', '.gif', '.webp', '.bmp', '.jpg', '.jpeg'];

    // Collect all image paths first
    const allImages = [];
    for (const [path, fileObj] of Object.entries(zip.files)) {
      if (fileObj.dir) continue;
      const ext = path.substring(path.lastIndexOf('.')).toLowerCase();
      if (imageExtensions.includes(ext)) {
        allImages.push(path);
      }
    }

    // Try to get images in reading order from OPF spine
    let orderedImages = [];
    let coverImagePath = null; // Track cover image
    try {
      // Find OPF file
      let opfPath = null;
      zip.forEach(p => { if (p.toLowerCase().endsWith('.opf')) opfPath = p; });

      if (opfPath) {
        const opfContent = await zip.files[opfPath].async('string');
        const opfDir = opfPath.includes('/') ? opfPath.substring(0, opfPath.lastIndexOf('/')) : '';

        // Detect cover image from OPF
        let coverId = null;
        let m;
        // Try 1: properties="cover-image"
        if (m = opfContent.match(/<item[^>]+id=["']([^"']+)["'][^>]+properties="[^"]*cover-image[^"]*"/)) coverId = m[1];
        if (!coverId && (m = opfContent.match(/<item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id=["']([^"']+)["']/))) coverId = m[1];
        // Try 2: meta name="cover" content="id"
        if (!coverId && (m = opfContent.match(/<meta\s+name=["']cover["']\s+content=["']([^"']+)["']/))) coverId = m[1];
        if (!coverId && (m = opfContent.match(/<meta\s+content=["']([^"']+)["']\s+name=["']cover["']/))) coverId = m[1];

        // Parse manifest to get id -> href mapping
        const manifest = {};
        const manifestRegex = /<item[^>]+id=["']([^"']+)["'][^>]+href=["']([^"']+)["'][^>]*>/gi;
        let match;
        while ((match = manifestRegex.exec(opfContent)) !== null) {
          const id = match[1];
          const href = match[2];
          const fullPath = opfDir ? opfDir + '/' + href : href;
          manifest[id] = fullPath;
          if (id === coverId) coverImagePath = fullPath;
        }
        // Also check reversed attribute order
        const manifestRegex2 = /<item[^>]+href=["']([^"']+)["'][^>]+id=["']([^"']+)["'][^>]*>/gi;
        while ((match = manifestRegex2.exec(opfContent)) !== null) {
          const href = match[1];
          const id = match[2];
          const fullPath = opfDir ? opfDir + '/' + href : href;
          manifest[id] = fullPath;
          if (id === coverId) coverImagePath = fullPath;
        }

        // Cover-page reconciliation — if the cover XHTML references a different
        // but byte-identical image, prefer the one actually displayed on the page.
        if (coverImagePath) {
          try {
            let coverXhtmlPath = null;
            const guideM = opfContent.match(/<(?:\w+:)?reference[^>]+type=["']cover["'][^>]+href=["']([^"']+)["']/i) ||
                            opfContent.match(/<(?:\w+:)?reference[^>]+href=["']([^"']+)["'][^>]+type=["']cover["']/i);
            if (guideM) {
              coverXhtmlPath = opfDir ? opfDir + '/' + decodeHref(guideM[1]) : decodeHref(guideM[1]);
            }
            if (!coverXhtmlPath) {
              const spineM = opfContent.match(/<(?:\w+:)?itemref[^>]+idref=["']([^"']+)["']/i);
              if (spineM && manifest[spineM[1]]) coverXhtmlPath = manifest[spineM[1]];
            }
            if (coverXhtmlPath && zip.files[coverXhtmlPath]) {
              const coverXhtml = await zip.files[coverXhtmlPath].async('string');
              const imgM = coverXhtml.match(/(?:src|xlink:href)=["']([^"']+)["']/i);
              if (imgM) {
                const href = imgM[1];
                const xDir = coverXhtmlPath.includes('/') ? coverXhtmlPath.substring(0, coverXhtmlPath.lastIndexOf('/')) : '';
                let pageImgPath = href.startsWith('../') ? xDir.split('/').slice(0, -1).join('/') + '/' + href.substring(3)
                                : href.startsWith('/') ? href.substring(1)
                                : xDir ? xDir + '/' + href : href;
                pageImgPath = pageImgPath.replace(/\/+/g, '/');
                for (const realPath of allImages) {
                  if (realPath === pageImgPath || realPath.endsWith('/' + href) || realPath.endsWith(href)) {
                    pageImgPath = realPath; break;
                  }
                }
                if (pageImgPath !== coverImagePath && allImages.includes(pageImgPath) && zip.files[pageImgPath]) {
                  const coverData = await zip.files[coverImagePath].async('arraybuffer');
                  const pageData = await zip.files[pageImgPath].async('arraybuffer');
                  if (coverData.byteLength === pageData.byteLength) {
                    const a = new Uint8Array(coverData);
                    const b = new Uint8Array(pageData);
                    let identical = true;
                    for (let i = 0; i < a.length; i++) { if (a[i] !== b[i]) { identical = false; break; } }
                    if (identical) coverImagePath = pageImgPath;
                  }
                }
              }
            }
          } catch (e) { /* non-critical */ }
        }

        // Parse spine to get reading order
        const spineOrder = [];
        const spineRegex = /<itemref[^>]+idref=["']([^"']+)["'][^>]*>/gi;
        while ((match = spineRegex.exec(opfContent)) !== null) {
          const idref = match[1];
          if (manifest[idref]) spineOrder.push(manifest[idref]);
        }

        // For each spine item (XHTML), extract images in order
        const seenImages = new Set();
        for (const xhtmlPath of spineOrder) {
          if (!zip.files[xhtmlPath]) continue;
          const xhtmlContent = await zip.files[xhtmlPath].async('string');
          const xhtmlDir = xhtmlPath.includes('/') ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf('/')) : '';

          // Find all image references
          const imgRegex = /(?:src|xlink:href)=["']([^"']+)["']/gi;
          while ((match = imgRegex.exec(xhtmlContent)) !== null) {
            let imgHref = match[1];
            // Skip non-image references
            if (!imageExtensions.some(ext => imgHref.toLowerCase().endsWith(ext))) continue;

            // Resolve relative path
            let imgPath;
            if (imgHref.startsWith('../')) {
              // Go up from xhtmlDir
              const parts = xhtmlDir.split('/');
              parts.pop();
              imgPath = parts.join('/') + '/' + imgHref.substring(3);
            } else if (imgHref.startsWith('/')) {
              imgPath = imgHref.substring(1);
            } else {
              imgPath = xhtmlDir ? xhtmlDir + '/' + imgHref : imgHref;
            }
            // Normalize path
            imgPath = imgPath.replace(/\/+/g, '/');

            // Check if this is actually an image in our list
            for (const realPath of allImages) {
              if (realPath === imgPath || realPath.endsWith('/' + imgHref) || realPath.endsWith(imgHref)) {
                if (!seenImages.has(realPath)) {
                  seenImages.add(realPath);
                  orderedImages.push(realPath);
                }
                break;
              }
            }
          }
        }

        // Add any remaining images that weren't in XHTML files (e.g., unused images)
        for (const imgPath of allImages) {
          if (!seenImages.has(imgPath)) {
            orderedImages.push(imgPath);
          }
        }
      }
    } catch (e) {
      console.warn('Failed to parse reading order, using default:', e);
    }

    // Fallback to alphabetical if parsing failed
    if (orderedImages.length === 0) {
      orderedImages = [...allImages].sort();
    }

    // Load image data in order
    const images = [];
    for (const path of orderedImages) {
      const data = await zip.files[path].async('arraybuffer');
      const blob = new Blob([data]);
      const dataUrl = URL.createObjectURL(blob);

      // Get dimensions
      const dims = await getImageDimensions(data);

      // Check if this is the cover image
      const isCover = (path === coverImagePath) ||
                      path.toLowerCase().includes('cover') && images.length === 0;

      // Check if this is a separator/ornament (skip cover check)
      const filename = path.split('/').pop();
      let isSeparator = false;
      if (!isCover) {
        try {
          isSeparator = await isSeparatorImage(dataUrl, dims.width, dims.height, filename);
        } catch (e) {
          console.warn('Separator check failed for', filename, e);
        }
      }

      // Tiny images (<200x200) are locked like separators
      const isTiny = (dims.width < 200 && dims.height < 200);

      // Images that fit screen can only rotate, not split
      const fitsScreen = (dims.width <= MAX_WIDTH && dims.height <= MAX_HEIGHT);

      // Split capability - no upscaling allowed
      // H-Split scales width to MAX_HEIGHT (long edge), so needs width >= MAX_HEIGHT
      // V-Split scales height to MAX_HEIGHT, so needs height >= MAX_HEIGHT
      const canHSplit = dims.width >= MAX_HEIGHT;
      const canVSplit = dims.height >= MAX_HEIGHT;

      images.push({
        path: path,
        name: filename,
        dataUrl: dataUrl,
        width: dims.width,
        height: dims.height,
        isCover: isCover,
        isSeparator: isSeparator || isTiny,
        fitsScreen: fitsScreen,
        canHSplit: canHSplit,
        canVSplit: canVSplit
      });
    }

    return images;
  }

  /**
   * Get image dimensions from array buffer
   */
  function getImageDimensions(data) {
    return new Promise((resolve, reject) => {
      const url = URL.createObjectURL(new Blob([data]));
      const img = new Image();
      img.onload = () => {
        URL.revokeObjectURL(url);
        resolve({ width: img.width, height: img.height });
      };
      img.onerror = () => {
        URL.revokeObjectURL(url);
        resolve({ width: 0, height: 0 });
      };
      img.src = url;
    });
  }

  /**
   * Check if image is a separator/ornament
   * Criteria: small size AND (filename match OR symmetric OR extreme aspect ratio)
   */
  async function isSeparatorImage(dataUrl, width, height, filename) {
    const MAX_DIMENSION = 150;
    const SYMMETRY_THRESHOLD = 0.85;

    // First check: must be small in at least one dimension
    const isSmall = (height < MAX_DIMENSION || width < MAX_DIMENSION);
    if (!isSmall) return false;

    // Filename hints (instant match if small + named correctly)
    const separatorNames = ['separator', 'divider', 'ornament', 'break', 'flourish', 'scene', 'divid', 'decor'];
    const lowerName = filename.toLowerCase();
    if (separatorNames.some(n => lowerName.includes(n))) return true;

    // Extreme aspect ratio check (>10:1 or <1:10) - these are definitely separators/lines
    const aspectRatio = width / height;
    if (aspectRatio > 10 || aspectRatio < 0.1) return true;

    // Symmetry check (skip for very thin images - too few pixels)
    if (width < 10 || height < 10) return true; // Very small = separator

    try {
      const isSymmetric = await checkHorizontalSymmetry(dataUrl, width, height, SYMMETRY_THRESHOLD);
      return isSymmetric;
    } catch (e) {
      console.warn('Symmetry check failed:', e);
      return false;
    }
  }

  /**
   * Check horizontal symmetry by comparing left and right halves
   */
  function checkHorizontalSymmetry(dataUrl, width, height, threshold) {
    return new Promise((resolve) => {
      const img = new Image();
      img.onload = () => {
        // Use small canvas for performance (max 100px wide)
        const scale = Math.min(1, 100 / width);
        const w = Math.max(2, Math.floor(width * scale));  // Minimum 2px
        const h = Math.max(1, Math.floor(height * scale)); // Minimum 1px

        const canvas = document.createElement('canvas');
        canvas.width = w;
        canvas.height = h;
        const ctx = canvas.getContext('2d');

        // Draw scaled image
        ctx.drawImage(img, 0, 0, w, h);
        const imageData = ctx.getImageData(0, 0, w, h);
        const pixels = imageData.data;

        // Compare left half with flipped right half
        const halfW = Math.floor(w / 2);
        let matchingPixels = 0;
        let totalPixels = 0;

        for (let y = 0; y < h; y++) {
          for (let x = 0; x < halfW; x++) {
            const leftIdx = (y * w + x) * 4;
            const rightIdx = (y * w + (w - 1 - x)) * 4;

            // Compare RGB (ignore alpha)
            const rDiff = Math.abs(pixels[leftIdx] - pixels[rightIdx]);
            const gDiff = Math.abs(pixels[leftIdx + 1] - pixels[rightIdx + 1]);
            const bDiff = Math.abs(pixels[leftIdx + 2] - pixels[rightIdx + 2]);

            // Allow some tolerance for JPEG artifacts (threshold of 30)
            if (rDiff < 30 && gDiff < 30 && bDiff < 30) {
              matchingPixels++;
            }
            totalPixels++;
          }
        }

        const symmetryScore = matchingPixels / totalPixels;
        resolve(symmetryScore >= threshold);
      };
      img.onerror = () => resolve(false);
      img.src = dataUrl;
    });
  }

  /**
   * Show image picker after EPUB file selection
   */
  async function showImagePicker(file) {
    // Lazily load jszip now that the optimizer is actually being used.
    try { await ensureJSZip(); } catch (e) { console.error(e); }
    // Check if JSZip is available
    if (typeof JSZip === 'undefined') {
      console.error('JSZip not loaded');
      alert('JSZip library not available. Conversion will proceed without image picker.');
      startConversionWithImageStates();
      return;
    }

    const pickerSection = document.getElementById('imagePickerSection');
    const imageGrid = document.getElementById('imageGrid');
    const countDisplay = document.getElementById('imagePickerCount');

    // Reset state
    imageStates = {};
    epubImagesCache = [];
    pendingConversionFile = file;

    // Extract images
    try {
      const images = await extractImagesForPreview(file);
      epubImagesCache = images;

      // Initialize all states to 0 (Normal)
      images.forEach(img => {
        imageStates[img.path] = 0;
      });

      // Build UI
      renderImageGrid();

      // Count images - covers and separators are locked
      const coverCount = images.filter(img => img.isCover).length;
      const separatorCount = images.filter(img => img.isSeparator).length;
      const lockedCount = coverCount + separatorCount;
      const selectableCount = images.length - lockedCount;

      if (lockedCount > 0) {
        const lockedParts = [];
        if (coverCount > 0) lockedParts.push(`${coverCount} cover`);
        if (separatorCount > 0) lockedParts.push(`${separatorCount} separator${separatorCount !== 1 ? 's' : ''}`);
        countDisplay.textContent = `${images.length} images (${selectableCount} selectable, ${lockedParts.join(', ')})`;
      } else {
        countDisplay.textContent = `${images.length} image${images.length !== 1 ? 's' : ''} (all selectable)`;
      }

      // Show picker, hide upload button, show start conversion button
      pickerSection.style.display = 'block';
      document.getElementById('uploadBtn').style.display = 'none';
      document.getElementById('startConversionBtn').style.display = 'block';
      // Enable two-column layout
      document.querySelector('#uploadModal .modal').classList.add('picker-mode');
      document.getElementById('pickerColumns').classList.add('picker-active');

    } catch (error) {
      console.error('Failed to extract images:', error);
      alert('Failed to preview images: ' + error.message + '\n\nConversion will proceed normally.');
      // Fallback: start conversion directly
      startConversionWithImageStates();
    }
  }

  /**
   * Render the image grid with current states
   */
  function renderImageGrid() {
    const imageGrid = document.getElementById('imageGrid');
    imageGrid.innerHTML = '';

    const stateLabels = ['', 'H-Split', 'V-Split', 'Rotate'];
    const stateClasses = ['state-0', 'state-1', 'state-2', 'state-3'];

    epubImagesCache.forEach(img => {
      // Cover images and separators are always locked (no splitting/rotation)
      const isCover = img.isCover;
      const isSeparator = img.isSeparator;
      const state = imageStates[img.path] || 0;

      const item = document.createElement('div');

      if (isCover) {
        // Cover image - locked, cannot be split
        item.className = 'image-item cover-locked';
        item.title = `${img.width}×${img.height} - Cover image (locked)`;
        item.innerHTML = `
          <span class="image-state-badge cover-badge">🔒</span>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
      } else if (isSeparator) {
        // Separator/ornament - locked, cannot be split
        item.className = 'image-item separator-locked';
        item.title = `${img.width}×${img.height} - Separator (locked)`;
        item.innerHTML = `
          <span class="image-state-badge separator-badge">✦</span>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
      } else {
        // All other images are selectable - all modes allowed

        // Determine which overlay to show based on state
        const showRotation = (state === 1 || state === 3); // H-Split or Rotate
        const showSplitLines = (state === 1 || state === 2); // H-Split or V-Split
        const rotateClass = showRotation ? (HANDEDNESS === 'right' ? 'rotate-cw' : 'rotate-ccw') : '';

        // Calculate actual number of parts for split preview
        let numParts = 1;
        if (showSplitLines) {
          let finalWidth;
          if (state === 1) {
            // H-Split: scale width to MAX_HEIGHT, rotate, then check width
            const scaledH = Math.round(img.height * (MAX_HEIGHT / img.width));
            finalWidth = scaledH; // After rotation, height becomes width
          } else {
            // V-Split: scale height to MAX_HEIGHT, then check width
            finalWidth = Math.round(img.width * (MAX_HEIGHT / img.height));
          }
          if (finalWidth > MAX_WIDTH) {
            const minOverlapPx = Math.round(MAX_WIDTH * (OVERLAP_PERCENT / 100));
            const maxStep = MAX_WIDTH - minOverlapPx;
            numParts = Math.ceil((finalWidth - minOverlapPx) / maxStep);
            if (numParts < 2) numParts = 2;
          }
        }

        // Generate split line elements (numParts - 1 lines at evenly distributed positions)
        let splitLinesHtml = '';
        if (showSplitLines && numParts > 1) {
          const lines = [];
          const splitClass = state === 1 ? 'split-h' : 'split-v';
          for (let i = 1; i < numParts; i++) {
            const pos = (i / numParts) * 100;
            lines.push(`<div class="split-line ${splitClass}" style="left:${pos}%"></div>`);
          }
          splitLinesHtml = `<div class="split-lines">${lines.join('')}</div>`;
        }

        // Build tooltip
        const stateText = stateLabels[state] || 'Normal';
        const partsText = numParts > 1 ? ` (${numParts} parts)` : '';

        item.className = `image-item ${stateClasses[state]} ${rotateClass}`.trim();
        item.onclick = () => cycleImageState(img.path);
        item.title = `${img.width}×${img.height} - ${stateText}${partsText}`;
        item.innerHTML = `
          <span class="image-state-badge">${stateLabels[state] || '•'}</span>
          <div class="image-preview-overlay">
            ${splitLinesHtml}
          </div>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
      }

      imageGrid.appendChild(item);
    });
  }

  /**
   * Cycle state for a single image
   * All non-locked images: 0 -> 1 -> 2 -> 3 -> 0
   */
  function cycleImageState(imagePath) {
    const currentState = imageStates[imagePath] || 0;
    imageStates[imagePath] = (currentState + 1) % 4;
    renderImageGrid();
  }

  /**
   * Apply state to all eligible images based on smart rules
   * 0 = Normal (all selectable images)
   * 1 = H-Split (landscapes that canHSplit)
   * 2 = V-Split (all images that canVSplit - portraits AND landscapes)
   * 3 = Rotate (landscapes that don't fit screen)
   */
  function applyStateToAll(state) {
    epubImagesCache.forEach(img => {
      // Skip locked images
      if (img.isCover || img.isSeparator) return;

      const canHSplit = img.canHSplit && !img.fitsScreen;
      const canVSplit = img.canVSplit && !img.fitsScreen;
      const isLandscape = img.width > img.height;

      if (state === 0) {
        // Normal - applies to all selectable
        imageStates[img.path] = 0;
      } else if (state === 1) {
        // H-Split - all landscapes that can H-Split
        if (isLandscape && canHSplit) {
          imageStates[img.path] = 1;
        }
      } else if (state === 2) {
        // V-Split - all images that can V-Split (portrait and landscape)
        if (canVSplit) {
          imageStates[img.path] = 2;
        }
      } else if (state === 3) {
        // Rotate - landscapes that exceed screen
        if (isLandscape && !img.fitsScreen) {
          imageStates[img.path] = 3;
        }
      }
    });
    renderImageGrid();
  }

  /**
   * Start conversion with configured image states
   */
  function startConversionWithImageStates() {
    // CrumBLE 4.5.4: clear the sticky cancel flag at run-entry. Otherwise a
    // prior cancelled run leaves operationCancelled=true and the first call
    // to convertEpubFile throws 'Cancelled by user' immediately. Same fix
    // applies to optimizeSelectedOnDevice -- whichever path the user took.
    if (typeof operationCancelled !== 'undefined') {
      try { operationCancelled = false; } catch (_) { /* not writable in some scopes */ }
    }
    // CrumBLE 4.5.4: snapshot the user's per-image picker choices so a
    // later retrySingleUpload() can skip the picker UI and head straight
    // to optimize+upload with the same prepared layout. The picker was
    // the slow/noisy interaction (30+ image dropdowns on a chapter-heavy
    // book); once the user committed, retries shouldn't make them redo
    // it just because the WS connection dropped mid-stream.
    if (pendingConversionFile && typeof cachedImageStatesByFile !== 'undefined') {
      const key = (pendingConversionFile.name || '') + '|' + (pendingConversionFile.size || 0);
      try {
        cachedImageStatesByFile.set(key, JSON.parse(JSON.stringify(imageStates)));
      } catch (_) { /* non-fatal: lose-cache fallback shows picker again on retry */ }
    }
    const pickerSection = document.getElementById('imagePickerSection');
    const uploadBtn = document.getElementById('uploadBtn');
    const startConversionBtn = document.getElementById('startConversionBtn');

    // Hide picker and start conversion button, remove two-column layout
    pickerSection.style.display = 'none';
    startConversionBtn.style.display = 'none';
    document.querySelector('#uploadModal .modal').classList.remove('picker-mode');
    document.getElementById('pickerColumns').classList.remove('picker-active');

    // Show upload button and trigger upload
    uploadBtn.style.display = 'block';
    uploadBtn.disabled = false;
    uploadFile();
  }

  /**
   * Get processing state for an image path
   * Returns 0 (Normal), 1 (H-Split), 2 (V-Split), or 3 (Rotate)
   */
  function getImageState(imagePath) {
    return imageStates[imagePath] || 0;
  }

  /**
   * Get state label for logging
   */
  function getStateLabel(state) {
    const labels = ['Normal', 'H-Split', 'V-Split', 'Rotate'];
    return labels[state] || 'Normal';
  }

// Format bytes to human-readable size (for logging)
function formatBytes(b) {
  if (!b) return '0 B';
  const k = 1024;
  const s = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(b) / Math.log(k));
  return (b / Math.pow(k, i)).toFixed(1) + ' ' + s[i];
}

// Get elapsed timestamp since log start
function getTimestamp() {
  if (!logStartTime) return '[00:00.0]';
  const elapsed = (Date.now() - logStartTime) / 1000;
  const mins = Math.floor(elapsed / 60).toString().padStart(2, '0');
  const secs = (elapsed % 60).toFixed(1).padStart(4, '0');
  return `[${mins}:${secs}]`;
}

// Main logging function
function log(message, type = '', tag = '') {
  const entry = document.createElement('div');
  entry.className = 'log-entry ' + type;

  // Timestamp
  const timestamp = document.createElement('span');
  timestamp.className = 'log-timestamp';
  timestamp.textContent = getTimestamp();
  entry.appendChild(timestamp);

  // Tag (if provided)
  if (tag) {
    const tagEl = document.createElement('span');
    tagEl.className = 'log-tag ' + tag.toLowerCase();
    tagEl.textContent = tag;
    entry.appendChild(tagEl);
  }

  // Message
  const msg = document.createElement('span');
  msg.className = 'log-message';
  msg.innerHTML = message;
  entry.appendChild(msg);

  logContainer.appendChild(entry);
  logContainer.scrollTop = logContainer.scrollHeight;
}

// Log image processing details
function logImage(name, origW, origH, origFormat, origSize, newW, newH, newSize, wasSplit = false, splitCount = 0, partsInfo = null, imageState = 0) {
  const saved = origSize - newSize;
  const savedPct = ((saved / origSize) * 100).toFixed(0);
  const dims = `${origW}×${origH}`;
  const newDims = `${newW}×${newH}`;

  // Get state label and color
  const stateLabels = ['', 'H-Split', 'V-Split', 'Rotate'];
  const stateColors = ['', '#3498db', '#e74c3c', '#9b59b6'];
  const stateLabel = stateLabels[imageState] || '';
  const stateColor = stateColors[imageState] || '';

  if (wasSplit) {
    conversionStats.splits++;
    conversionStats.splitParts += splitCount;
    // Build parts detail string
    let partsDetail = '';
    if (partsInfo && partsInfo.length > 0) {
      const baseName = name.replace(/\.[^.]+$/, '');
      partsDetail = partsInfo.map(p =>
        `${baseName}${p.suffix}.jpg (${p.width}×${p.height}, ${formatBytes(p.size)})`
      ).join(', ');
    }
    const savedInfo = saved > 0 ? `, <span style="color:#27ae60">-${savedPct}%</span>` : '';
    const stateIndicator = imageState > 0 ? ` <span style="color:${stateColor};font-weight:600;">[${stateLabel}]</span>` : '';
    log(`<strong>${escapeHtml(name)}</strong>${stateIndicator} <span class="log-detail">(${dims} ${origFormat.toUpperCase()}, ${formatBytes(origSize)}) → ${splitCount} parts${savedInfo}</span>`, '', 'SPLIT');
    if (partsDetail) {
      log(`<span class="log-detail" style="margin-left: 20px;">↳ ${partsDetail}</span>`, '', '');
    }
  } else {
    conversionStats.images++;
    const stateIndicator = imageState > 0 ? ` <span style="color:${stateColor};font-weight:600;">[${stateLabel}]</span>` : '';
    const detail = saved > 0
      ? `<span class="log-detail">(${dims} → ${newDims}, ${formatBytes(origSize)} → ${formatBytes(newSize)}, <span style="color:#27ae60">-${savedPct}%</span>)</span>`
      : `<span class="log-detail">(${dims} → ${newDims}, ${formatBytes(newSize)})</span>`;
    log(`<strong>${escapeHtml(name)}</strong>${stateIndicator} ${detail}`, '', 'CONVERT');
  }
}

// Log fix applied
function logFix(type, detail) {
  conversionStats.fixes++;
  log(`${type}: <span class="log-detail">${detail}</span>`, 'success', 'FIX');
}

// Log skipped item
function logSkip(name, reason) {
  conversionStats.skipped++;
  log(`${escapeHtml(name)} <span class="log-detail">(${reason})</span>`, '', 'SKIP');
}

// Log error
function logError(message) {
  conversionStats.errors++;
  log(message, 'error', 'ERROR');
}

// Log summary table
function logSummary(originalSize, newSize, timeElapsed) {
  const saved = originalSize - newSize;
  const savedPct = ((saved / originalSize) * 100).toFixed(1);
  const totalImages = conversionStats.images + conversionStats.splits;
  const totalOutput = conversionStats.images + conversionStats.splitParts;

  const summaryHtml = `
    <div class="log-summary">
      <div class="log-summary-title">📊 Conversion Summary</div>
      <table class="log-summary-table">
        <tr><td>Images found</td><td class="highlight">${totalImages}</td></tr>
        <tr><td>Images processed</td><td>${totalOutput}${conversionStats.splitParts > conversionStats.splits ? ` (+${conversionStats.splitParts - conversionStats.splits} from splits)` : ''}</td></tr>
        <tr><td>EPUB repairs</td><td>${conversionStats.fixes > 0 ? conversionStats.fixes + ' fixes applied' : 'None needed'}</td></tr>
        ${conversionStats.errors > 0 ? `<tr><td>Errors</td><td style="color:#e74c3c">${conversionStats.errors}</td></tr>` : ''}
        <tr><td>Original size</td><td>${formatBytes(originalSize)}</td></tr>
        <tr><td>Optimized size</td><td>${formatBytes(newSize)}</td></tr>
        <tr><td>Saved</td><td class="${saved > 0 ? 'saved' : 'increased'}">${saved > 0 ? formatBytes(saved) + ' (' + savedPct + '%)' : '+' + formatBytes(-saved)}</td></tr>
        <tr><td>Time</td><td>${timeElapsed.toFixed(1)}s</td></tr>
      </table>
    </div>
  `;
  logContainer.insertAdjacentHTML('beforeend', summaryHtml);
  logContainer.scrollTop = logContainer.scrollHeight;
}

// Clear log
function clearLog() {
  logContainer.innerHTML = '';
  logStartTime = Date.now();
  conversionStats = { images: 0, splits: 0, splitParts: 0, fixes: 0, skipped: 0, errors: 0, originalSize: 0, newSize: 0 };
}

// Start batch logging mode
function startBatchLog(fileCount) {
  isBatchMode = true;
  batchStartTime = Date.now();
  batchLogEntries = [];
  batchStats = { filesProcessed: 0, filesSucceeded: 0, filesFailed: 0, totalImages: 0, totalSplits: 0, totalFixes: 0, totalErrors: 0, totalOriginalSize: 0, totalNewSize: 0 };
  clearLog();
  logContainer.innerHTML = ''; // Clear display
  log(`Starting batch conversion: ${fileCount} file(s)`, '', 'INFO');
}

// Save current file's log to batch entries
function saveToFileBatchLog(fileName, succeeded, originalSize = 0, newSize = 0) {
  if (!isBatchMode) return;

  const entries = Array.from(logContainer.querySelectorAll('.log-entry'));
  batchLogEntries.push({
    fileName: fileName,
    succeeded: succeeded,
    entries: entries,
    stats: { ...conversionStats }
  });

  // Update batch stats
  batchStats.filesProcessed++;
  if (succeeded) {
    batchStats.filesSucceeded++;
  } else {
    batchStats.filesFailed++;
  }
  batchStats.totalImages += conversionStats.images;
  batchStats.totalSplits += conversionStats.splits;
  batchStats.totalFixes += conversionStats.fixes;
  batchStats.totalErrors += conversionStats.errors;
  // Defaults of 0 keep failure-path callers safe — files that never
  // produced a converted blob contribute nothing to the totals.
  batchStats.totalOriginalSize += originalSize;
  batchStats.totalNewSize += newSize;

  // Clear for next file
  logContainer.innerHTML = '';
  conversionStats = { images: 0, splits: 0, splitParts: 0, fixes: 0, skipped: 0, errors: 0, originalSize: 0, newSize: 0 };
}

// Finalize batch log and export
function finalizeBatchLog() {
  if (!isBatchMode) return;

  const batchTime = (Date.now() - batchStartTime) / 1000;

  // Build consolidated log display
  logContainer.innerHTML = '';
  log(`Starting batch conversion: ${batchStats.filesProcessed} file(s)`, '', 'INFO');

  // Add all file entries
  batchLogEntries.forEach((fileLog, index) => {
    const fileHeader = document.createElement('div');
    fileHeader.className = 'log-entry';
    fileHeader.style.marginTop = index > 0 ? '15px' : '5px';
    fileHeader.style.borderTop = index > 0 ? '1px solid var(--border-color)' : 'none';
    fileHeader.style.paddingTop = index > 0 ? '10px' : '0';
    fileHeader.innerHTML = `<span class="log-timestamp"></span><span class="log-message"><strong>${escapeHtml(fileLog.fileName)}</strong> — ${fileLog.succeeded ? '<span style="color:#27ae60">✓ Success</span>' : '<span style="color:#e74c3c">✗ Failed</span>'}</span>`;
    logContainer.appendChild(fileHeader);

    fileLog.entries.forEach(entry => {
      const clone = entry.cloneNode(true);
      logContainer.appendChild(clone);
    });
  });

  // Aggregate size totals: only emit rows when at least one file was successfully
  // converted (totalOriginalSize stays 0 for batches where conversion was off or
  // every file fell back to original upload).
  const totalSaved = batchStats.totalOriginalSize - batchStats.totalNewSize;
  const totalSavedPct = batchStats.totalOriginalSize > 0
    ? ((totalSaved / batchStats.totalOriginalSize) * 100).toFixed(1)
    : '0.0';
  const sizeRowsHtml = batchStats.totalOriginalSize > 0 ? `
        <tr><td>Total original</td><td>${formatBytes(batchStats.totalOriginalSize)}</td></tr>
        <tr><td>Total optimised</td><td>${formatBytes(batchStats.totalNewSize)}</td></tr>
        <tr><td>Total saved</td><td class="${totalSaved > 0 ? 'saved' : 'increased'}">${
          totalSaved > 0
            ? `${formatBytes(totalSaved)} (${totalSavedPct}%)`
            : `+${formatBytes(-totalSaved)}`
        }</td></tr>` : '';

  // Add batch summary
  const batchSummaryHtml = `
    <div class="log-summary">
      <div class="log-summary-title">📊 Batch Conversion Summary</div>
      <table class="log-summary-table">
        <tr><td>Files processed</td><td class="highlight">${batchStats.filesProcessed}</td></tr>
        <tr><td>Successful</td><td style="color:#27ae60">${batchStats.filesSucceeded}</td></tr>
        <tr><td>Failed</td><td style="${batchStats.filesFailed > 0 ? '#e74c3c' : '#7f8c8d'}">${batchStats.filesFailed}</td></tr>
        <tr><td>Total images processed</td><td>${batchStats.totalImages}</td></tr>
        <tr><td>Total splits</td><td>${batchStats.totalSplits}</td></tr>
        <tr><td>Total fixes applied</td><td>${batchStats.totalFixes}</td></tr>
        ${batchStats.totalErrors > 0 ? `<tr><td>Total errors</td><td style="color:#e74c3c">${batchStats.totalErrors}</td></tr>` : ''}${sizeRowsHtml}
        <tr><td>Total time</td><td>${batchTime.toFixed(1)}s</td></tr>
      </table>
    </div>
  `;
  logContainer.insertAdjacentHTML('beforeend', batchSummaryHtml);
  logContainer.scrollTop = logContainer.scrollHeight;

  // Auto-export if checkbox is checked
  if (exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, true); // isBatch = true
    }, 200);
  }

  // Reset batch mode
  isBatchMode = false;
  batchLogEntries = [];
}

// Show/hide log section
function showLog() {
  logSection.classList.add('visible');
}

function hideLog() {
  logSection.classList.remove('visible');
}

// Generate standardized log filename with date
function generateLogFilename(isBatch = false) {
  const now = new Date();
  const date = now.toISOString().split('T')[0]; // YYYY-MM-DD
  const time = now.toTimeString().split(' ')[0].replace(/:/g, '-'); // HH-MM-SS
  const prefix = isBatch ? 'batch' : 'epub';
  return `${prefix}-conversion-log-${date}_${time}.txt`;
}

// Export log as text file (can be called automatically)
function exportLogToFile(filename = null, isBatch = false) {
  // Use standardized filename if none provided
  if (!filename) {
    filename = generateLogFilename(isBatch);
  }
  // Extract text from log entries
  const entries = logContainer.querySelectorAll('.log-entry');
  let logText = `CrossInk Reader ${crosspointVersion} - EPUB Conversion Log\n`;
  logText += `Generated: ${new Date().toLocaleString()}\n`;
  logText += `${'='.repeat(60)}\n\n`;

  entries.forEach(entry => {
    const timestamp = entry.querySelector('.log-timestamp')?.textContent || '';
    const tag = entry.querySelector('.log-tag')?.textContent || '';
    const message = entry.querySelector('.log-message')?.textContent || entry.textContent;

    if (tag) {
      logText += `${timestamp} [${tag}] ${message}\n`;
    } else {
      logText += `${timestamp} ${message}\n`;
    }
  });

  // Extract summary table if present
  const summary = logContainer.querySelector('.log-summary');
  if (summary) {
    logText += `\n${'='.repeat(60)}\n`;
    const title = summary.querySelector('.log-summary-title')?.textContent || 'Summary';
    logText += `${title}\n`;
    logText += `${'-'.repeat(40)}\n`;

    const rows = summary.querySelectorAll('tr');
    rows.forEach(row => {
      const cells = row.querySelectorAll('td');
      if (cells.length >= 2) {
        logText += `${cells[0].textContent.padEnd(25)} ${cells[1].textContent}\n`;
      }
    });
  }

  // Create download link
  const blob = new Blob([logText], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename || `epub-conversion-log-${Date.now()}.txt`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

/** Escape a string for safe insertion into XML attribute values / text content. */
function xmlEscape(str) {
  return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function escapeRegex(str) { return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

/**
 * Decode a URI-encoded href (e.g., "my%20image.jpg" → "my image.jpg").
 * Handles double-encoding gracefully.
 */
function decodeHref(href) {
  try { return decodeURIComponent(href); }
  catch (e) { return href; }
}

/**
 * Safely read a text file from the zip, handling BOM and encoding.
 * Strips UTF-8 BOM. Detects encoding from XML declaration or meta tag.
 */
async function safeReadText(fileObj) {
  const raw = await fileObj.async('uint8array');

  // Detect and strip UTF-8 BOM (EF BB BF)
  let offset = 0;
  if (raw.length >= 3 && raw[0] === 0xEF && raw[1] === 0xBB && raw[2] === 0xBF) {
    offset = 3;
  }

  // Try UTF-8 first (vast majority of EPUBs)
  const utf8 = new TextDecoder('utf-8', { fatal: true });
  try {
    return utf8.decode(raw.subarray(offset));
  } catch (e) { /* not valid UTF-8 */ }

  // Peek at XML declaration or meta charset for encoding hint
  const ascii = new TextDecoder('ascii', { fatal: false }).decode(raw.subarray(offset, offset + 512));
  const encodingMatch = ascii.match(/encoding=["']([^"']+)["']/i) ||
                        ascii.match(/charset=["']?([^"'\s;]+)/i);
  const encoding = encodingMatch ? encodingMatch[1].toLowerCase() : 'windows-1252';

  try {
    return new TextDecoder(encoding, { fatal: false }).decode(raw.subarray(offset));
  } catch (e) {
    // Last resort: lossy latin1
    return new TextDecoder('iso-8859-1', { fatal: false }).decode(raw.subarray(offset));
  }
}

/**
 * Find the canonical OPF path by parsing META-INF/container.xml.
 * Falls back to scanning for any .opf file.
 */
async function findOPFPath(zip) {
  try {
    const containerPath = Object.keys(zip.files).find(p => p.toLowerCase() === 'meta-inf/container.xml');
    if (containerPath) {
      const containerXml = await zip.files[containerPath].async('string');
      const match = containerXml.match(/<rootfile[^>]+full-path=["']([^"']+)["']/i);
      if (match && zip.files[match[1]]) return match[1];
    }
  } catch (e) { /* fall through */ }
  let fallback = null;
  zip.forEach(p => { if (!fallback && p.toLowerCase().endsWith('.opf')) fallback = p; });
  return fallback;
}

/**
 * Resolve a relative href against a base file path.
 * Handles multiple ../, ./, absolute /, and bare relative paths.
 */
function resolvePath(basePath, href) {
  if (href.startsWith('/')) return href.substring(1);
  href = href.replace(/^\.\//, '');
  const baseDir = basePath.includes('/') ? basePath.substring(0, basePath.lastIndexOf('/')) : '';
  const baseParts = baseDir ? baseDir.split('/') : [];
  const hrefParts = href.split('/');
  while (hrefParts.length > 0 && hrefParts[0] === '..') {
    hrefParts.shift();
    if (baseParts.length > 0) baseParts.pop();
  }
  const resolved = [...baseParts, ...hrefParts].join('/');
  return resolved.replace(/\/+/g, '/');
}

/**
 * Serialize an XML doc back to string, preserving the original <?xml?> declaration
 * and cleaning up XMLSerializer namespace prefix noise (xmlns:ns0 etc).
 */
function safeSerialize(doc, originalContent) {
  let result = new XMLSerializer().serializeToString(doc);

  // Restore <?xml?> declaration if original had one
  if (originalContent && /^\s*<\?xml\b/.test(originalContent) && !/^\s*<\?xml\b/.test(result)) {
    const declMatch = originalContent.match(/^\s*(<\?xml[^?]*\?>)/);
    if (declMatch) result = declMatch[1] + '\n' + result;
  }

  // Clean up XMLSerializer namespace prefix noise (xmlns:ns0="..." ns0:attr="...")
  result = result.replace(/ xmlns:ns\d+="[^"]*"/g, '');
  result = result.replace(/ ns\d+:/g, ' ');

  return result;
}

function protectWhitespaceOnlyTextNodes(content) {
  const preserved = [];
  const tokenPrefix = '__CROSSINK_PRESERVE_WS_';
  const protectedContent = content.replace(/>([\s\u00a0]+)</g, (_, whitespace) => {
    const token = `${tokenPrefix}${preserved.length}__`;
    preserved.push(whitespace);
    return `>${token}<`;
  });

  return {
    content: protectedContent,
    restore(serialized) {
      return serialized.replace(new RegExp(`${escapeRegex(tokenPrefix)}(\\d+)__`, 'g'), (match, indexText) => {
        const index = Number(indexText);
        return Number.isInteger(index) && index >= 0 && index < preserved.length ? preserved[index] : match;
      });
    }
  };
}

/**
 * Extract main identifier from OPF for NCX sync. DOMParser with regex fallback.
 */
function extractIdentifier(opfContent) {
  let mainIdentifier = null;
  try {
    const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const pkg = doc.getElementsByTagNameNS('*', 'package')[0];
      const uid = pkg ? pkg.getAttribute('unique-identifier') : null;
      if (uid) {
        const el = [...doc.getElementsByTagNameNS('*', 'identifier')].find(e => e.getAttribute('id') === uid);
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
      if (!mainIdentifier) {
        const el = doc.getElementsByTagNameNS('*', 'identifier')[0];
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
    }
  } catch (e) { /* fall through to regex */ }
  if (!mainIdentifier) {
    const uniqueIdMatch = opfContent.match(/<(?:\w+:)?package[^>]*unique-identifier=["']([^"']+)["']/i);
    if (uniqueIdMatch) {
      const idRegex = new RegExp(`<dc:identifier[^>]*id=["']${uniqueIdMatch[1]}["'][^>]*>([^<]+)</dc:identifier>`, 'i');
      const idMatch = opfContent.match(idRegex);
      if (idMatch) mainIdentifier = idMatch[1].trim();
    }
    if (!mainIdentifier) {
      const firstIdMatch = opfContent.match(/<dc:identifier[^>]*>([^<]+)</i);
      if (firstIdMatch) mainIdentifier = firstIdMatch[1].trim();
    }
  }
  return mainIdentifier;
}

/**
 * Sync NCX dtb:uid with the given identifier. DOMParser with regex fallback.
 */
function syncNCXIdentifier(ncxText, mainIdentifier) {
  if (!mainIdentifier) return ncxText;
  let t = ncxText;
  try {
    const doc = new DOMParser().parseFromString(t, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const meta = [...doc.getElementsByTagNameNS('*', 'meta')].find(m => m.getAttribute('name') === 'dtb:uid');
      if (meta) {
        meta.setAttribute('content', mainIdentifier);
        t = safeSerialize(doc, ncxText);
      }
    }
  } catch (e) {
    t = t.replace(/<meta\s+name=["']dtb:uid["']\s+content=["'][^"']*["']\s*\/?>/gi, `<meta name="dtb:uid" content="${xmlEscape(mainIdentifier)}"/>`);
  }
  return t;
}

/**
 * Fix OPF content: fix media-types, strip svg properties,
 * update split image manifest entries, ensure cover meta.
 * DOMParser with regex fallback.
 */
function fixOPF(opfText, opfOriginal, opfDir, splitImages = {}) {
  let t = opfText;

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(t, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('OPF parse failed');

    const items = [...doc.getElementsByTagNameNS('*', 'item')];
    const manifestEl = doc.getElementsByTagNameNS('*', 'manifest')[0];

    // Fix media-types for converted images
    for (const item of items) {
      const href = item.getAttribute('href') || '';
      const type = item.getAttribute('media-type') || '';
      if (href.endsWith('.jpg') && type.match(/^image\/(png|gif|webp|bmp)$/)) {
        item.setAttribute('media-type', 'image/jpeg');
      }
    }

    // Remove 'svg' from properties
    for (const item of items) {
      const props = item.getAttribute('properties') || '';
      if (props.includes('svg')) {
        const newProps = props.split(/\s+/).filter(p => p !== 'svg').join(' ').trim();
        if (newProps) item.setAttribute('properties', newProps);
        else item.removeAttribute('properties');
      }
    }

    // Update split image hrefs and add manifest entries for parts
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + '/') ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
      const part1Href = origHrefJpg.replace(/\.jpg$/i, '_part1.jpg');

      for (const item of items) {
        const h = item.getAttribute('href') || '';
        if (h === origHref || h === origHrefJpg || decodeHref(h) === origHref || decodeHref(h) === origHrefJpg) {
          item.setAttribute('href', part1Href);
          break;
        }
      }

      if (manifestEl) {
        const ns = manifestEl.namespaceURI || 'http://www.idpf.org/2007/opf';
        for (let j = 1; j < parts.length; j++) {
          const p = parts[j];
          const href = opfDir && p.path.startsWith(opfDir + '/') ? p.path.substring(opfDir.length + 1) : p.path;
          const newItem = doc.createElementNS(ns, 'item');
          newItem.setAttribute('id', `img-${p.id}`);
          newItem.setAttribute('href', href);
          newItem.setAttribute('media-type', 'image/jpeg');
          manifestEl.appendChild(newItem);
        }
      }
    }

    t = safeSerialize(doc, opfOriginal);
  } catch (e) {
    // Regex fallback
    t = t.replace(/(<(?:\w+:)?item\b[^>]*href="[^"]+\.jpg"[^>]*)media-type="image\/(png|gif|webp|bmp)"/g, '$1media-type="image/jpeg"');
    t = t.replace(/(<(?:\w+:)?item\b[^>]*)media-type="image\/(png|gif|webp|bmp)"([^>]*href="[^"]+\.jpg")/g, '$1media-type="image/jpeg"$3');
    t = t.replace(/\s+svg(?=["'\s>])/g, '');
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + '/') ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
      const part1Href = origHrefJpg.replace(/\.jpg$/i, '_part1.jpg');
      const origImgRegex = new RegExp(`(href=["'])(${escapeRegex(origHref)}|${escapeRegex(origHrefJpg)})(["'])`, 'gi');
      t = t.replace(origImgRegex, `$1${part1Href}$3`);
      let adds = '';
      for (let j = 1; j < parts.length; j++) {
        const p = parts[j];
        const href = opfDir && p.path.startsWith(opfDir + '/') ? p.path.substring(opfDir.length + 1) : p.path;
        adds += `<item id="img-${xmlEscape(p.id)}" href="${xmlEscape(href)}" media-type="image/jpeg"/>\n`;
      }
      if (adds && t.includes('</manifest>')) t = t.replace('</manifest>', adds + '</manifest>');
    }
  }

  // Ensure cover meta
  const cm = ensureCoverMeta(t);
  if (cm.fixed) t = cm.o;

  return t;
}

// Fix SVG cover - converts SVG-wrapped covers to plain HTML img tags
function fixSvgCover(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };
  if (!content.includes('calibre:cover') && !content.includes('name="cover"') && !content.includes('<title>Cover</title>')) return { c: content, fixed: false, count: 0 };

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
    }

    // Find SVG elements - check both standard and namespaced variants
    let imgHref = null;
    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    // Try to find all SVG elements
    const svgs = [
      ...doc.getElementsByTagName('svg'),
      ...doc.getElementsByTagNameNS(svgNS, 'svg'),
      ...doc.getElementsByTagName('svg:svg')
    ];

    for (const svg of svgs) {
      // Find image element inside - try all variants
      const imageEl = svg.getElementsByTagName('image')[0] ||
                      svg.getElementsByTagNameNS(svgNS, 'image')[0] ||
                      svg.getElementsByTagName('svg:image')[0];

      if (imageEl) {
        imgHref = imageEl.getAttributeNS(xlinkNS, 'href') ||
                  imageEl.getAttribute('xlink:href') ||
                  imageEl.getAttribute('href');
        if (imgHref) break;
      }
    }

    if (!imgHref) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      imgHref = m[1];
    }

    return {
      c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${imgHref}"/></section></body>
</html>`,
      fixed: true,
      count: 1
    };
  } catch (e) {
    // Fallback to regex
    const m = content.match(/xlink:href=["']([^"']+)["']/);
    if (!m) return { c: content, fixed: false, count: 0 };
    return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
  }
}

// Fix SVG-wrapped images - unwrap SVG and replace with plain img
function fixSvgWrappedImages(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };

  try {
    const whitespaceGuard = protectWhitespaceOnlyTextNodes(content);
    const parser = new DOMParser();
    const doc = parser.parseFromString(whitespaceGuard.content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      let fixedCount = 0;
      const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
      const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
      return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
    }

    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    const svgElements = [...doc.querySelectorAll('svg'), ...doc.getElementsByTagNameNS(svgNS, 'svg')];
    const uniqueSvgs = [...new Set(svgElements)];
    let fixedCount = 0;

    for (const svg of uniqueSvgs) {
      const imageEl = svg.querySelector('image[*|href]') || svg.getElementsByTagNameNS(svgNS, 'image')[0] || svg.getElementsByTagNameNS('*', 'image')[0];
      if (!imageEl) continue;
      const href = imageEl.getAttributeNS(xlinkNS, 'href') || imageEl.getAttribute('xlink:href') || imageEl.getAttribute('href');
      if (!href) continue;
      const width = imageEl.getAttribute('width') || svg.getAttribute('width');
      const height = imageEl.getAttribute('height') || svg.getAttribute('height');
      const img = doc.createElementNS('http://www.w3.org/1999/xhtml', 'img');
      img.setAttribute('src', href);
      img.setAttribute('alt', '');
      img.setAttribute('style', 'max-width:100%;height:auto');
      if (width) img.setAttribute('width', width);
      if (height) img.setAttribute('height', height);
      svg.parentNode.replaceChild(img, svg);
      fixedCount++;
    }

    if (fixedCount === 0) return { c: content, fixed: false, count: 0 };
    return { c: whitespaceGuard.restore(safeSerialize(doc, whitespaceGuard.content)), fixed: true, count: fixedCount };

  } catch (e) {
    // Fallback to regex
    let fixedCount = 0;
    const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
    const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
    return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
  }
}

// Ensure cover meta tag exists in OPF — DOMParser with regex fallback
function ensureCoverMeta(opfString) {
  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(opfString, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('Parse failed');

    // Find cover image id: properties="cover-image", or id/href containing "cover"
    let coverId = null;
    const items = [...doc.getElementsByTagNameNS('*', 'item')];
    for (const item of items) {
      const props = item.getAttribute('properties') || '';
      const id = item.getAttribute('id') || '';
      const type = item.getAttribute('media-type') || '';
      if (!type.startsWith('image/')) continue;
      if (props.includes('cover-image')) { coverId = id; break; }
    }
    if (!coverId) {
      for (const item of items) {
        const id = item.getAttribute('id') || '';
        const href = item.getAttribute('href') || '';
        const type = item.getAttribute('media-type') || '';
        if (!type.startsWith('image/')) continue;
        if (id.toLowerCase().includes('cover') || href.toLowerCase().includes('cover')) { coverId = id; break; }
      }
    }
    if (!coverId) return { o: opfString, fixed: false };

    // Find or create <meta name="cover" content="..."/>
    const metas = [...doc.getElementsByTagNameNS('*', 'meta')];
    const coverMeta = metas.find(m => m.getAttribute('name') === 'cover');
    if (coverMeta) {
      if (coverMeta.getAttribute('content') === coverId) return { o: opfString, fixed: false };
      coverMeta.setAttribute('content', coverId);
    } else {
      const metadata = doc.getElementsByTagNameNS('*', 'metadata')[0];
      if (!metadata) return { o: opfString, fixed: false };
      const ns = metadata.namespaceURI || 'http://www.idpf.org/2007/opf';
      const newMeta = doc.createElementNS(ns, 'meta');
      newMeta.setAttribute('name', 'cover');
      newMeta.setAttribute('content', coverId);
      metadata.appendChild(newMeta);
    }
    return { o: safeSerialize(doc, opfString), fixed: true };
  } catch (e) {
    // Regex fallback
    return ensureCoverMetaRegex(opfString);
  }
}

function ensureCoverMetaRegex(o) {
  let coverId = null, m;
  if (!coverId && (m = o.match(/<\w+:?item[^>]+id="([^"]+)"[^>]+properties="[^"]*cover-image[^"]*"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id="([^"]+)"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]+)"[^>]*href="[^"]*cover[^"]*"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*href="[^"]*cover[^"]*"[^>]*id="([^"]+)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]*cover[^"]*)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*media-type="image\/[^"]*"[^>]*id="([^"]*cover[^"]*)"/i))) coverId = m[1];
  if (!coverId) return { o, fixed: false };
  const metaMatch = o.match(/<\w+:?meta\s+name=["']cover["']\s+content=["']([^"']+)["']/i) || o.match(/<\w+:?meta\s+content=["']([^"']+)["']\s+name=["']cover["']/i);
  if (metaMatch) {
    if (metaMatch[1] === coverId && !metaMatch[1].includes('/')) return { o, fixed: false };
    const esc = xmlEscape(coverId);
    o = o.replace(/<\w+:?meta\s+name=["']cover["']\s+content=["'][^"']+["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    o = o.replace(/<\w+:?meta\s+content=["'][^"']+["']\s+name=["']cover["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    return { o, fixed: true };
  }
  const idx = o.indexOf('</metadata>');
  if (idx !== -1) return { o: o.substring(0, idx) + `    <meta name="cover" content="${xmlEscape(coverId)}"/>\n  </metadata>` + o.substring(idx + 11), fixed: true };
  return { o, fixed: false };
}

// Apply grayscale to canvas image data
function applyGrayscale(ctx, width, height) {
  if (!ENABLE_GRAYSCALE) return;
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    // Alpha-blend against white background before grayscaling (handles transparent PNGs)
    const a = data[i + 3] / 255;
    const blendedR = data[i] * a + 255 * (1 - a);
    const blendedG = data[i + 1] * a + 255 * (1 - a);
    const blendedB = data[i + 2] * a + 255 * (1 - a);
    const gray = Math.round(blendedR * 0.299 + blendedG * 0.587 + blendedB * 0.114);
    data[i] = gray; data[i + 1] = gray; data[i + 2] = gray; data[i + 3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
}

// Process single image - returns array of {data, suffix} objects
const IMAGE_LOAD_TIMEOUT_MS = 30000; // 30 second timeout for image loading
async function processImage(data, imageState = 0, imagePath = '') {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(new Blob([data]));
    const img = new Image();
    const origSize = data.byteLength;
    // Set up timeout to handle cases where image never loads
    const timeoutId = setTimeout(() => {
      URL.revokeObjectURL(url);
      reject(new Error('Image load timeout'));
    }, IMAGE_LOAD_TIMEOUT_MS);

    img.onload = async () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      const origW = img.width, origH = img.height;

      // imageState: 0=Normal, 1=H-Split (CW/CCW), 2=V-Split, 3=Rotate & Fit
      // ========================================================================
      // STATE 1: H-Split (Rotate + Split) - EXACT COPY FROM index.html
      // Step 1: Scale WIDTH to 800px (keep aspect ratio)
      // Step 2: Rotate 90° CW or CCW based on HANDEDNESS
      // Step 3: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      if (imageState === 1) {
        // Step 1: Scale WIDTH to 800 (this is the key difference!)
        const scale = MAX_HEIGHT / origW;  // 800 / origW
        const scaledW = MAX_HEIGHT;  // 800
        const scaledH = Math.round(origH * scale);

        const scaledCanvas = document.createElement('canvas');
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext('2d');
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = 'high';
        scaledCtx.fillStyle = '#FFF';
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(img, 0, 0, origW, origH, 0, 0, scaledW, scaledH);

        // Step 2: Rotate 90° CW or CCW
        const rotW = scaledH;
        const rotH = scaledW;  // 800

        const rotCanvas = document.createElement('canvas');
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext('2d');
        rotCtx.fillStyle = '#FFF';
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === 'right';
        if (isClockwise) {
          // Rotate 90° CW
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          // Rotate 90° CCW
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(scaledCanvas, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0); // Reset transform
        applyGrayscale(rotCtx, rotW, rotH);

        // Step 3: If WIDTH > 480, split vertically
        if (rotW <= MAX_WIDTH) {
          const blob = await new Promise(res => rotCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: rotW, finalH: rotH, finalSize: arrBuf.byteLength, imageState: 1 }
          });
        } else {
          // Split by WIDTH (vertical cuts) - from RIGHT to LEFT for CW, LEFT to RIGHT for CCW
          const parts = [];
          const maxW = MAX_WIDTH;  // 480

          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((rotW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((rotW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x;
            if (isClockwise) {
              // CW: right to left - start from right edge
              x = rotW - maxW - (i * step);
            } else {
              // CCW: left to right - start from left edge
              x = i * step;
            }
            // Clamp to valid range
            x = Math.max(0, Math.min(x, rotW - maxW));
            positions.push(x);
          }

          // Ensure first and last positions are at edges
          if (isClockwise) {
            positions[0] = rotW - maxW; // First part at right edge
            positions[numParts - 1] = 0; // Last part at left edge
          } else {
            positions[0] = 0; // First part at left edge
            positions[numParts - 1] = rotW - maxW; // Last part at right edge
          }

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement('canvas');
            partCanvas.width = partW;
            partCanvas.height = rotH;
            const partCtx = partCanvas.getContext('2d');
            // Clear canvas first
            partCtx.fillStyle = '#FFFFFF';
            partCtx.fillRect(0, 0, partW, rotH);
            // Draw the slice
            partCtx.drawImage(rotCanvas, x, 0, partW, rotH, 0, 0, partW, rotH);

            const blob = await new Promise(res => partCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({ data: arrBuf, suffix: `_part${i + 1}`, width: partW, height: rotH, size: arrBuf.byteLength });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: true, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 1 }
          });
        }
      }
      // ========================================================================
      // STATE 2: V-Split (Vertical Split, no rotation)
      // Step 1: Scale HEIGHT to 800px (up or down)
      // Step 2: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      else if (imageState === 2) {
        // ALWAYS scale height to 800 (up or down)
        const scale = MAX_HEIGHT / origH;  // 800 / origH
        const scaledW = Math.round(origW * scale);
        const scaledH = MAX_HEIGHT;  // Always 800

        const scaledCanvas = document.createElement('canvas');
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext('2d');
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = 'high';
        scaledCtx.fillStyle = '#FFF';
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(img, 0, 0, origW, origH, 0, 0, scaledW, scaledH);
        applyGrayscale(scaledCtx, scaledW, scaledH);

        // Check if split needed
        if (scaledW <= MAX_WIDTH) {
          const blob = await new Promise(res => scaledCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: scaledW, height: scaledH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: scaledW, finalH: scaledH, finalSize: arrBuf.byteLength, imageState: 2 }
          });
        } else {
          // Split by WIDTH (vertical cuts) - LEFT to RIGHT (natural reading order)
          const parts = [];
          const maxW = MAX_WIDTH;

          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((scaledW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((scaledW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x = i * step;
            // Clamp to valid range
            x = Math.max(0, Math.min(x, scaledW - maxW));
            positions.push(x);
          }
          // Ensure last position is at right edge
          positions[0] = 0;
          positions[numParts - 1] = scaledW - maxW;

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement('canvas');
            partCanvas.width = partW;
            partCanvas.height = scaledH;
            const partCtx = partCanvas.getContext('2d');
            // Clear canvas first
            partCtx.fillStyle = '#FFFFFF';
            partCtx.fillRect(0, 0, partW, scaledH);
            // Draw the slice
            partCtx.drawImage(scaledCanvas, x, 0, partW, scaledH, 0, 0, partW, scaledH);

            const blob = await new Promise(res => partCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({ data: arrBuf, suffix: `_part${i + 1}`, width: partW, height: scaledH, size: arrBuf.byteLength });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: false, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 2 }
          });
        }
      }
      // ========================================================================
      // STATE 3: Rotate & Fit (Rotate 90°, then scale to fit 480x800, no split)
      // ========================================================================
      else if (imageState === 3) {
        // Step 1: Rotate 90° based on handedness
        const rotW = origH;
        const rotH = origW;

        const rotCanvas = document.createElement('canvas');
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext('2d');
        rotCtx.fillStyle = '#FFF';
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === 'right';
        if (isClockwise) {
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(img, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0);

        // Step 2: Scale to fit 480x800 (if needed)
        const fitsInScreen = rotW <= MAX_WIDTH && rotH <= MAX_HEIGHT;

        if (fitsInScreen) {
          // Already fits after rotation - just apply grayscale
          applyGrayscale(rotCtx, rotW, rotH);
          const blob = await new Promise(res => rotCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: rotW, finalH: rotH, finalSize: arrBuf.byteLength, imageState: 3 }
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / rotW, MAX_HEIGHT / rotH);
          const newW = Math.round(rotW * scale);
          const newH = Math.round(rotH * scale);

          const scaledCanvas = document.createElement('canvas');
          scaledCanvas.width = newW;
          scaledCanvas.height = newH;
          const scaledCtx = scaledCanvas.getContext('2d');
          scaledCtx.imageSmoothingEnabled = true;
          scaledCtx.imageSmoothingQuality = 'high';
          scaledCtx.fillStyle = '#FFF';
          scaledCtx.fillRect(0, 0, newW, newH);
          scaledCtx.drawImage(rotCanvas, 0, 0, newW, newH);
          applyGrayscale(scaledCtx, newW, newH);

          const blob = await new Promise(res => scaledCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: newW, height: newH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: newW, finalH: newH, finalSize: arrBuf.byteLength, imageState: 3 }
          });
        }
      }
      // ========================================================================
      // STATE 0: Normal processing (scale to fit, no split/rotation)
      // ========================================================================
      else {
        // Normal processing: check if scaling is needed
        const fitsInScreen = origW <= MAX_WIDTH && origH <= MAX_HEIGHT;

        if (fitsInScreen) {
          // Image already fits - just convert to JPEG with grayscale
          const c = document.createElement('canvas');
          c.width = origW;
          c.height = origH;
          const ctx = c.getContext('2d');
          ctx.fillStyle = '#FFF';
          ctx.fillRect(0, 0, origW, origH);
          ctx.drawImage(img, 0, 0);
          applyGrayscale(ctx, origW, origH);

          const blob = await new Promise(res => c.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: origW, height: origH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: origW, finalH: origH, finalSize: arrBuf.byteLength, imageState: 0 }
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
          const newW = Math.round(origW * scale);
          const newH = Math.round(origH * scale);

          const c = document.createElement('canvas');
          c.width = newW;
          c.height = newH;
          const ctx = c.getContext('2d');
          ctx.imageSmoothingEnabled = true;
          ctx.imageSmoothingQuality = 'high';
          ctx.fillStyle = '#FFF';
          ctx.fillRect(0, 0, newW, newH);
          ctx.drawImage(img, 0, 0, newW, newH);
          applyGrayscale(ctx, newW, newH);

          const blob = await new Promise(res => c.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: newW, height: newH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: newW, finalH: newH, finalSize: arrBuf.byteLength, imageState: 0 }
          });
        }
      }
    };
    img.onerror = () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      reject(new Error('Image load failed'));
    };
    img.src = url;
  });
}

// Convert EPUB file - returns converted blob
// CrumBLE: bake a .pxc pixel cache from JPEG bytes at the device's fitted display
// dimensions, matching the on-device format: uint16 LE width, uint16 LE height,
// then 2-bit pixels (4 per byte, MSB-first; 0=black .. 3=white, Floyd-Steinberg
// dithered to 4 levels to match the device decoder's shading quality). The
// device only accepts a .pxc whose dimensions match its own scale-to-fit within
// 1px, so we reproduce that exact formula (integer truncation, scale never > 1).
// Returns a Uint8Array, or null if the image can't be baked.
async function bakePxc(jpegBytes, viewportW, viewportH) {
  const url = URL.createObjectURL(new Blob([jpegBytes], { type: 'image/jpeg' }));
  try {
    const img = await new Promise((resolve, reject) => {
      const im = new Image();
      im.onload = () => resolve(im);
      im.onerror = () => reject(new Error('decode failed'));
      im.src = url;
    });
    const iw = img.naturalWidth || img.width;
    const ih = img.naturalHeight || img.height;
    if (!iw || !ih) return null;
    let scaleX = (iw > viewportW) ? viewportW / iw : 1.0;
    let scaleY = (ih > viewportH) ? viewportH / ih : 1.0;
    let scale = Math.min(scaleX, scaleY);
    if (scale > 1.0) scale = 1.0;
    const dw = Math.trunc(iw * scale);
    const dh = Math.trunc(ih * scale);
    if (dw < 1 || dh < 1) return null;
    const canvas = document.createElement('canvas');
    canvas.width = dw;
    canvas.height = dh;
    const ctx = canvas.getContext('2d');
    ctx.drawImage(img, 0, 0, dw, dh);
    const px = ctx.getImageData(0, 0, dw, dh).data;  // RGBA
    const bytesPerRow = (dw + 3) >> 2;
    const out = new Uint8Array(4 + bytesPerRow * dh);
    out[0] = dw & 0xFF; out[1] = (dw >> 8) & 0xFF;
    out[2] = dh & 0xFF; out[3] = (dh >> 8) & 0xFF;

    // Floyd-Steinberg dither to 4 levels (0=black .. 3=white, same convention the
    // device decoder uses). Plain gray/85 quantization made mid-grays collapse to
    // flat black/white blocks -- baked images looked far harsher than the device's
    // own dithered decode. Error diffusion spreads the quantization error so
    // gradients/shading render as smooth 4-level dither (and even in 1-bit BW mode
    // it reads as a halftone instead of solid black).
    const grayBuf = new Float32Array(dw * dh);
    for (let i = 0, p = 0; i < dw * dh; i++, p += 4) {
      grayBuf[i] = (px[p] * 77 + px[p + 1] * 150 + px[p + 2] * 29) / 256;  // device luma weights
    }
    const levels = new Uint8Array(dw * dh);
    for (let y = 0; y < dh; y++) {
      for (let x = 0; x < dw; x++) {
        const idx = y * dw + x;
        let g = grayBuf[idx];
        if (g < 0) g = 0; else if (g > 255) g = 255;
        let level = Math.round(g / 85);  // nearest of 0,85,170,255
        if (level > 3) level = 3;
        levels[idx] = level;
        const err = g - level * 85;
        if (x + 1 < dw) grayBuf[idx + 1] += err * (7 / 16);
        if (y + 1 < dh) {
          if (x > 0) grayBuf[idx + dw - 1] += err * (3 / 16);
          grayBuf[idx + dw] += err * (5 / 16);
          if (x + 1 < dw) grayBuf[idx + dw + 1] += err * (1 / 16);
        }
      }
    }

    let o = 4;
    for (let y = 0; y < dh; y++) {
      const rowBase = y * dw;
      for (let xb = 0; xb < bytesPerRow; xb++) {
        let b = 0;
        for (let k = 0; k < 4; k++) {
          const x = xb * 4 + k;
          const level = (x < dw) ? levels[rowBase + x] : 3;  // padding -> white (ignored beyond width)
          b |= (level & 3) << (6 - k * 2);  // MSB-first: pixel 0 in bits 6-7
        }
        out[o++] = b;
      }
    }
    return out;
  } catch (e) {
    return null;
  } finally {
    URL.revokeObjectURL(url);
  }
}

// v18.9.9.291 CrumBLE Option A: bake a 1-bit BMP thumbnail from cover image
// bytes for a specific device thumb size. Output matches the on-device
// Bitmap reader's expectations:
//   - Standard BMP header (14 bytes file + 40 bytes DIB + 8 bytes palette = 62)
//   - 1 bit per pixel, top-down (negative height in DIB header)
//   - Palette index 0 = black (0,0,0), index 1 = white (255,255,255)
//   - Rows padded to multiples of 4 bytes
// Contain-fit: preserves aspect ratio, letterboxes the shorter dimension.
// Floyd-Steinberg dithered to 1-bit BW so the baked thumb looks close to
// what the on-device generator would produce.
// Returns a Uint8Array, or null on decode failure.
async function bakeCoverThumbBmp(imageBytes, targetW, targetH) {
  const blob = new Blob([imageBytes]);  // let browser sniff format
  const url = URL.createObjectURL(blob);
  try {
    const img = await new Promise((resolve, reject) => {
      const im = new Image();
      im.onload = () => resolve(im);
      im.onerror = () => reject(new Error('cover decode failed'));
      im.src = url;
    });
    const iw = img.naturalWidth || img.width;
    const ih = img.naturalHeight || img.height;
    if (!iw || !ih) return null;

    // Contain-fit into (targetW, targetH). Preserves aspect; letterboxes
    // the shorter dimension so the whole cover is visible even for weird
    // ratios (mangas, wide landscape covers, etc.).
    const scale = Math.min(targetW / iw, targetH / ih);
    const dw = Math.max(1, Math.round(iw * scale));
    const dh = Math.max(1, Math.round(ih * scale));
    const offX = Math.floor((targetW - dw) / 2);
    const offY = Math.floor((targetH - dh) / 2);

    const canvas = document.createElement('canvas');
    canvas.width = targetW;
    canvas.height = targetH;
    const ctx = canvas.getContext('2d');
    // White letterbox background so unused stripes render as blank white
    // on the e-ink panel rather than reading as unrelated art.
    ctx.fillStyle = '#FFFFFF';
    ctx.fillRect(0, 0, targetW, targetH);
    ctx.drawImage(img, offX, offY, dw, dh);
    const px = ctx.getImageData(0, 0, targetW, targetH).data;  // RGBA

    // Convert to grayscale (device luma weights, matches bakePxc).
    const grayBuf = new Float32Array(targetW * targetH);
    for (let i = 0, p = 0; i < targetW * targetH; i++, p += 4) {
      grayBuf[i] = (px[p] * 77 + px[p + 1] * 150 + px[p + 2] * 29) / 256;
    }

    // Floyd-Steinberg dither to 1-bit BW (threshold at 128).
    const bits = new Uint8Array(targetW * targetH);
    for (let y = 0; y < targetH; y++) {
      for (let x = 0; x < targetW; x++) {
        const idx = y * targetW + x;
        let g = grayBuf[idx];
        if (g < 0) g = 0; else if (g > 255) g = 255;
        const bit = g >= 128 ? 1 : 0;  // 1 = white in BMP palette
        bits[idx] = bit;
        const err = g - (bit ? 255 : 0);
        if (x + 1 < targetW) grayBuf[idx + 1] += err * (7 / 16);
        if (y + 1 < targetH) {
          if (x > 0) grayBuf[idx + targetW - 1] += err * (3 / 16);
          grayBuf[idx + targetW] += err * (5 / 16);
          if (x + 1 < targetW) grayBuf[idx + targetW + 1] += err * (1 / 16);
        }
      }
    }

    // BMP file layout:
    //   14 bytes file header
    //   40 bytes BITMAPINFOHEADER
    //    8 bytes palette (2 entries × BGRA)
    //   payload: (targetW+31)/32 * 4 bytes per row × targetH
    const bytesPerRow = ((targetW + 31) >> 5) << 2;  // round up to 4-byte mult
    const imageSize = bytesPerRow * targetH;
    const fileSize = 62 + imageSize;
    const buf = new Uint8Array(fileSize);
    let o = 0;
    // File header
    buf[o++] = 0x42; buf[o++] = 0x4D;  // 'BM'
    buf[o++] = fileSize & 0xFF; buf[o++] = (fileSize >> 8) & 0xFF;
    buf[o++] = (fileSize >> 16) & 0xFF; buf[o++] = (fileSize >> 24) & 0xFF;
    buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;  // reserved
    buf[o++] = 62; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;  // pixel data offset
    // DIB header (BITMAPINFOHEADER)
    buf[o++] = 40; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;  // header size
    buf[o++] = targetW & 0xFF; buf[o++] = (targetW >> 8) & 0xFF;
    buf[o++] = (targetW >> 16) & 0xFF; buf[o++] = (targetW >> 24) & 0xFF;
    // Height: negative = top-down (matches on-device writeBmpHeader1bit)
    const h = -targetH;
    buf[o++] = h & 0xFF; buf[o++] = (h >> 8) & 0xFF;
    buf[o++] = (h >> 16) & 0xFF; buf[o++] = (h >> 24) & 0xFF;
    buf[o++] = 1; buf[o++] = 0;   // color planes
    buf[o++] = 1; buf[o++] = 0;   // bits per pixel = 1
    buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;  // BI_RGB
    buf[o++] = imageSize & 0xFF; buf[o++] = (imageSize >> 8) & 0xFF;
    buf[o++] = (imageSize >> 16) & 0xFF; buf[o++] = (imageSize >> 24) & 0xFF;
    // 2835 pixels-per-meter (~72 DPI) both axes
    buf[o++] = 0x13; buf[o++] = 0x0B; buf[o++] = 0; buf[o++] = 0;
    buf[o++] = 0x13; buf[o++] = 0x0B; buf[o++] = 0; buf[o++] = 0;
    buf[o++] = 2; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;  // colorsUsed=2
    buf[o++] = 2; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;  // colorsImportant=2
    // Palette: BGRA
    buf[o++] = 0x00; buf[o++] = 0x00; buf[o++] = 0x00; buf[o++] = 0x00;  // black
    buf[o++] = 0xFF; buf[o++] = 0xFF; buf[o++] = 0xFF; buf[o++] = 0x00;  // white
    // Pixel data: MSB-first within each byte, first pixel of row in bit 7.
    for (let y = 0; y < targetH; y++) {
      const rowBase = y * targetW;
      const rowOut = o + y * bytesPerRow;
      for (let x = 0; x < targetW; x++) {
        if (bits[rowBase + x]) {
          buf[rowOut + (x >> 3)] |= (0x80 >> (x & 7));
        }
      }
    }
    return buf;
  } catch (e) {
    console.warn('bakeCoverThumbBmp failed:', e);
    return null;
  } finally {
    URL.revokeObjectURL(url);
  }
}

// v18.9.9.291: device thumb sizes we ship pre-baked. Keep in sync with
// device-side cell sizes; the device falls back to on-device PNG decode
// when it asks for a size that's not in this list, so it's safe to trim.
const COVER_THUMB_SIZES = [
  { w: 200, h: 390 },   // Lyra Flow side cover
  { w: 296, h: 468 },   // Lyra Flow center cover
  { w: 120, h: 176 },   // Recent Books Grid typical
];

// CrumBLE optimizer preflight modal. Before BT-bake or prebake locks the
// book's layout to a specific reader-settings snapshot, surface that
// snapshot to the user so they can verify (and edit) the values that will
// be embedded in the manifest -- and, on edit, push the corrected SETTINGS
// back to the device so the first cold open finds the prebake's
// fingerprint matching reality.
//
// Returns a Promise that resolves with the final renderInfo to use for the
// bake (potentially edited and re-fetched from the device after a save),
// or rejects with an Error('Cancelled by user') if the user backs out.
async function showOptimizerPreflightModal(renderInfo, fileName) {
  // Enum tables. Indexes match the CrossPointSettings.h enums byte-for-byte
  // -- the device validates the same ranges in handleSaveReaderSettings, so
  // any drift here would be caught server-side. Keep in sync with that file.
  //
  // Font-family is special: the dropdown carries STRING tags rather than
  // numeric values so we can represent both built-in fonts ("builtin:N")
  // and SD-card fonts ("sd:<family-name>") in the same select. Built-in
  // tags map to numeric fontFamily on save; SD tags map to
  // sdFontFamilyName. See the confirm.onclick handler below.
  // CrumBLE 4.2: built-in font list comes from /api/builtin-fonts so the
  // slim binaries (which OMIT Lexend Deca and/or CharE-Ink) don't offer
  // fonts that aren't actually on the device. Fallback to the legacy
  // hardcoded list if the endpoint is missing (older firmware).
  const FONT_FAMILY = [];
  try {
    const builtinsResp = await fetch('/api/builtin-fonts');
    if (builtinsResp.ok) {
      const builtinsData = await builtinsResp.json();
      for (const b of (builtinsData.builtins || [])) {
        if (b && typeof b.value === 'number' && typeof b.label === 'string') {
          FONT_FAMILY.push({ v: `builtin:${b.value}`, label: b.label });
        }
      }
    }
  } catch (e) {
    console.warn('Preflight: /api/builtin-fonts unavailable; using legacy hardcoded list', e);
  }
  if (FONT_FAMILY.length === 0) {
    FONT_FAMILY.push({ v: 'builtin:0', label: 'Lexend Deca' });
    FONT_FAMILY.push({ v: 'builtin:1', label: 'Bitter' });
    FONT_FAMILY.push({ v: 'builtin:2', label: 'CharE-Ink' });
  }

  // CrumBLE 4.1.1: pull SD-card font families from the device so users
  // with a custom .cpfont can confirm / pick it in this dialog. Missing
  // or failed /api/fonts response just falls through to built-ins only.
  // CrumBLE 4.2: also keep the per-family `sizes` array around for the
  // reactive size-dropdown rebuild below -- each SD font has its own
  // point-size list (whatever <Family>_<size>.cpfont files exist) and
  // the size dropdown should reflect what the user actually has.
  const SD_FONT_FAMILIES = {};  // name -> { sizes: [pt, pt, ...] }
  try {
    const fontsResp = await fetch('/api/fonts');
    if (fontsResp.ok) {
      const fontsData = await fontsResp.json();
      for (const family of (fontsData.families || [])) {
        if (family && typeof family.name === 'string' && family.name.length > 0) {
          FONT_FAMILY.push({ v: `sd:${family.name}`, label: `${family.name} (SD)` });
          SD_FONT_FAMILIES[family.name] = {
            sizes: Array.isArray(family.sizes) ? family.sizes.slice() : [],
          };
        }
      }
    }
  } catch (e) {
    console.warn('Preflight: /api/fonts unavailable; SD-card font families will be missing from the picker', e);
  }
  // CrumBLE 4.2: use the device-reported availableFontSizes so we don't
  // offer sizes the firmware doesn't ship (env:tiny OMITs several --
  // picking a missing one would silently fall back to a different size
  // and break the prebake fingerprint). renderInfo.availableFontSizes
  // arrives from /api/reader-render-info as
  // [{value: <storage-index>, pointSize: <int>}, ...] sorted by
  // smallest point size. Fall back to the legacy hardcoded list if the
  // field is missing (older firmware).
  let FONT_SIZE;
  if (Array.isArray(renderInfo.availableFontSizes) && renderInfo.availableFontSizes.length > 0) {
    const ptToName = (pt) => {
      if (pt <= 8) return 'Tiny';
      if (pt <= 10) return 'Small';
      if (pt <= 12) return 'Medium';
      if (pt <= 14) return 'Large';
      if (pt <= 16) return 'Extra Large';
      if (pt <= 18) return 'XL';
      return 'Huge';
    };
    FONT_SIZE = renderInfo.availableFontSizes
      .slice()
      .sort((a, b) => (a.pointSize | 0) - (b.pointSize | 0))
      .map((s) => ({ v: s.value | 0, label: `${ptToName(s.pointSize | 0)} (${s.pointSize | 0}pt)` }));
  } else {
    FONT_SIZE = [
      { v: 0, label: 'Tiny (8pt)' },
      { v: 1, label: 'Small (10pt)' },
      { v: 2, label: 'Medium (12pt)' },
      { v: 3, label: 'Large (14pt)' },
      { v: 4, label: 'Extra Large (16pt)' },
      { v: 5, label: 'Teensy (6pt)' },
      { v: 6, label: 'Huge (20pt)' },
    ];
  }
  const ORIENTATION = [
    { v: 0, label: 'Portrait' },
    { v: 1, label: 'Landscape (CW)' },
    { v: 3, label: 'Landscape (CCW)' },
  ];
  const LINE_SPACING = [
    { v: 0, label: 'Tight' },
    { v: 1, label: 'Normal' },
    { v: 2, label: 'Wide' },
  ];
  // CrumBLE 4.4: was a bool, now a 3-way enum mirroring LINE_SPACING.
  const PARAGRAPH_SPACING = [
    { v: 0, label: 'Tight' },
    { v: 1, label: 'Normal' },
    { v: 2, label: 'Wide' },
  ];
  const PARAGRAPH_ALIGNMENT = [
    { v: 0, label: 'Justified' },
    { v: 1, label: 'Left' },
    { v: 2, label: 'Center' },
    { v: 3, label: 'Right' },
  ];
  const IMAGE_RENDERING = [
    { v: 0, label: 'Show images' },
    { v: 1, label: 'Placeholder' },
    { v: 2, label: 'Suppress' },
  ];

  return new Promise((resolve, reject) => {
    const overlay = document.createElement('div');
    overlay.style.cssText =
      'position:fixed;inset:0;background:rgba(0,0,0,0.5);display:flex;align-items:center;' +
      'justify-content:center;z-index:10000;font-family:-apple-system,system-ui,sans-serif;';
    const dialog = document.createElement('div');
    dialog.style.cssText =
      'background:#fff;color:#222;border-radius:12px;max-width:560px;width:90%;max-height:85vh;overflow-y:auto;' +
      'padding:24px;box-shadow:0 12px 32px rgba(0,0,0,0.2);';
    const header = document.createElement('div');
    header.innerHTML =
      '<h2 style="margin:0 0 6px;font-size:20px;color:#222">Settings for this bake?</h2>' +
      '<p style="margin:0 0 10px;color:#555;font-size:14px;line-height:1.5">' +
      'Optimizing this book takes a few minutes longer (depending on length and images). ' +
      '<strong>Anything you change here only affects this bake for this book</strong>. ' +
      'If the bake target ends up different from your current device settings, ' +
      'it will offer you to switch when opening the book.</p>' +
      '<p style="margin:0 0 16px;color:#555;font-size:14px;line-height:1.5">' +
      'Note for CJK fonts, you must use <strong>Normal</strong> Line Spacing and Paragraph Spacing ' +
      'or the book will not be readable.</p>';
    dialog.appendChild(header);

    const settingNote = document.createElement('p');
    settingNote.style.cssText = 'margin:0 0 10px;font-size:13px;color:#777';
    settingNote.textContent = `Book: ${fileName}`;
    dialog.appendChild(settingNote);

    const form = document.createElement('div');
    form.style.cssText = 'display:grid;grid-template-columns:1fr 1fr;gap:10px 14px;margin:8px 0 16px;';

    const makeSelect = (label, key, options, current) => {
      const wrap = document.createElement('label');
      wrap.style.cssText = 'display:flex;flex-direction:column;font-size:13px;color:#444';
      const span = document.createElement('span');
      span.style.cssText = 'margin-bottom:3px;font-weight:500';
      span.textContent = label;
      const sel = document.createElement('select');
      sel.dataset.key = key;
      sel.dataset.current = current;
      sel.style.cssText = 'padding:6px 8px;border:1px solid #c2c2c2;border-radius:4px;font-size:14px;background:#fff;color:#222';
      for (const opt of options) {
        const o = document.createElement('option');
        o.value = opt.v;
        o.textContent = opt.label;
        if (opt.v === current) o.selected = true;
        sel.appendChild(o);
      }
      wrap.appendChild(span);
      wrap.appendChild(sel);
      form.appendChild(wrap);
    };
    const makeNumber = (label, key, current, min, max) => {
      const wrap = document.createElement('label');
      wrap.style.cssText = 'display:flex;flex-direction:column;font-size:13px;color:#444';
      const span = document.createElement('span');
      span.style.cssText = 'margin-bottom:3px;font-weight:500';
      span.textContent = label;
      const inp = document.createElement('input');
      inp.type = 'number';
      inp.min = String(min);
      inp.max = String(max);
      inp.value = String(current);
      inp.dataset.key = key;
      inp.dataset.current = String(current);
      inp.style.cssText = 'padding:6px 8px;border:1px solid #c2c2c2;border-radius:4px;font-size:14px;background:#fff;color:#222';
      wrap.appendChild(span);
      wrap.appendChild(inp);
      form.appendChild(wrap);
    };
    const makeBool = (label, key, current) => {
      const wrap = document.createElement('label');
      wrap.style.cssText =
        'display:flex;align-items:center;gap:8px;font-size:13px;color:#444;cursor:pointer';
      const inp = document.createElement('input');
      inp.type = 'checkbox';
      inp.dataset.key = key;
      inp.dataset.current = String(current ? 1 : 0);
      inp.checked = !!current;
      inp.style.cssText = 'width:18px;height:18px;cursor:pointer';
      const span = document.createElement('span');
      span.textContent = label;
      wrap.appendChild(inp);
      wrap.appendChild(span);
      form.appendChild(wrap);
    };

    // CrumBLE 4.1.1: pick the current font tag based on whether an SD
    // font is selected (sdFontFamilyName non-empty) or a built-in one.
    const currentFontTag = (renderInfo.sdFontFamilyName && renderInfo.sdFontFamilyName.length > 0)
      ? `sd:${renderInfo.sdFontFamilyName}`
      : `builtin:${renderInfo.fontFamily | 0}`;
    makeSelect('Font family', 'fontFamily', FONT_FAMILY, currentFontTag);
    makeSelect('Font size', 'fontSize', FONT_SIZE, renderInfo.fontSize | 0);

    // CrumBLE 4.2: when the user switches font family, repopulate the
    // size dropdown with options appropriate for the new family. Built-in
    // fonts share one shipped set (FONT_SIZE from /api/builtin-fonts);
    // each SD-card family has its OWN size list (only the
    // <Family>_<size>.cpfont files that actually exist). Without this,
    // the size dropdown sticks with Bitter's sizes even after picking an
    // SD font, and the user picks a "size" that doesn't have a .cpfont
    // file -- prebake then fails or produces wrong-metric layout.
    const familySel = form.querySelector('select[data-key="fontFamily"]');
    const sizeSel = form.querySelector('select[data-key="fontSize"]');
    const rebuildSizeOptions = (familyTag) => {
      let opts = FONT_SIZE;  // default to built-in sizes
      if (familyTag && familyTag.startsWith('sd:')) {
        const familyName = familyTag.substring(3);
        const family = SD_FONT_FAMILIES[familyName];
        if (family && family.sizes.length > 0) {
          opts = family.sizes
            .slice()
            .sort((a, b) => (a | 0) - (b | 0))
            .map((pt, i) => ({ v: i, label: `${pt}pt` }));
        }
      }
      const prevSelected = sizeSel.value;
      while (sizeSel.firstChild) sizeSel.removeChild(sizeSel.firstChild);
      let restored = false;
      for (const opt of opts) {
        const o = document.createElement('option');
        o.value = opt.v;
        o.textContent = opt.label;
        if (String(opt.v) === String(prevSelected)) {
          o.selected = true;
          restored = true;
        }
        sizeSel.appendChild(o);
      }
      if (!restored && sizeSel.firstChild) sizeSel.firstChild.selected = true;
    };
    familySel.addEventListener('change', () => rebuildSizeOptions(familySel.value));
    // Initial population may need to apply if device's saved font is SD.
    rebuildSizeOptions(currentFontTag);
    makeSelect('Orientation', 'orientation', ORIENTATION, renderInfo.orientation | 0);
    makeNumber('Screen margin (px)', 'screenMargin', renderInfo.screenMargin | 0, 0, 50);
    makeSelect('Line spacing', 'lineSpacing', LINE_SPACING, renderInfo.lineSpacing | 0);
    makeSelect('Paragraph alignment', 'paragraphAlignment', PARAGRAPH_ALIGNMENT, renderInfo.paragraphAlignment | 0);
    makeSelect('Image rendering', 'imageRendering', IMAGE_RENDERING, renderInfo.imageRendering | 0);
    makeSelect('Paragraph spacing', 'extraParagraphSpacing', PARAGRAPH_SPACING, renderInfo.extraParagraphSpacing | 0);
    makeBool('Force paragraph indents', 'forceParagraphIndents', renderInfo.forceParagraphIndents | 0);
    makeBool('Hyphenation', 'hyphenationEnabled', renderInfo.hyphenationEnabled | 0);
    makeBool('Embedded CSS', 'embeddedStyle', renderInfo.embeddedStyle | 0);
    makeBool('Bionic reading', 'bionicReadingEnabled', renderInfo.bionicReadingEnabled | 0);
    makeBool('Guide reading', 'guideReadingEnabled', renderInfo.guideReadingEnabled | 0);
    dialog.appendChild(form);

    const buttons = document.createElement('div');
    buttons.style.cssText = 'display:flex;gap:10px;justify-content:flex-end;margin-top:8px';
    const cancel = document.createElement('button');
    cancel.textContent = 'Cancel';
    cancel.style.cssText =
      'padding:9px 18px;border:1px solid #c2c2c2;background:#fafafa;border-radius:6px;' +
      'font-size:14px;cursor:pointer;color:#444';
    const confirm = document.createElement('button');
    confirm.textContent = 'Looks good, optimize';
    confirm.style.cssText =
      'padding:9px 18px;border:1px solid #2c7a3f;background:#2c7a3f;color:#fff;border-radius:6px;' +
      'font-size:14px;cursor:pointer;font-weight:600';
    buttons.appendChild(cancel);
    buttons.appendChild(confirm);
    dialog.appendChild(buttons);
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);

    const close = () => { try { document.body.removeChild(overlay); } catch (e) { /* gone already */ } };
    cancel.onclick = () => { close(); reject(new Error('Cancelled by user')); };
    // CrumBLE 4.5.5+: treat a click on the dimmed overlay as "user wants to
    // cancel" ONLY when the click STARTED on the overlay -- i.e. a real click
    // OUTSIDE the dialog. A text-selection drag that started inside the
    // dialog and released past its edge also delivers a `click` event whose
    // target is `overlay`, so the previous handler treated highlighting text
    // as a cancellation and rejected the modal promise with "Cancelled by
    // user". Field report: user was selecting body text in this modal to
    // copy into a message, drag released on the overlay, modal disappeared,
    // and the underlying optimize flow showed "Finished with errors" --
    // confusing both because they hadn't intended to cancel and because
    // they didn't think they'd started anything yet.
    //
    // Tracking mousedown lets us distinguish: a same-target press-then-
    // release on the overlay is a real outside click; a drag that
    // started elsewhere isn't.
    let overlayMouseDownOnOverlay = false;
    overlay.addEventListener('mousedown', (e) => { overlayMouseDownOnOverlay = (e.target === overlay); });
    overlay.onclick = (e) => {
      const wasOverlayClick = (e.target === overlay) && overlayMouseDownOnOverlay;
      overlayMouseDownOnOverlay = false;
      if (wasOverlayClick) { close(); reject(new Error('Cancelled by user')); }
    };

    confirm.onclick = async () => {
      // CrumBLE 4.5.5: bake-time-only overrides. POSTs to /api/save-
      // reader-settings with dryRun:true so the device computes derived
      // values (fontId, lineCompression, viewport, emSize) for the
      // hypothetical change WITHOUT persisting. Resolves with the
      // dry-run response as the bake's renderInfo. The device's actual
      // reader settings stay where the user left them -- if the bake
      // target differs, the device's existing fontId-mismatch prompt
      // kicks in on first open. Previously this branch POSTed the same
      // updates WITHOUT dryRun, mutating SETTINGS every time the user
      // touched the picker; one report was "I picked LXGW for one CJK
      // book and now my device's default font is LXGW too." Worst case
      // was the fontSize medium/large dropdown silently rewriting the
      // device's reading size for everyone.
      const updates = { dryRun: true };
      form.querySelectorAll('[data-key]').forEach(el => {
        const key = el.dataset.key;
        const val = (el.type === 'checkbox') ? (el.checked ? '1' : '0') : el.value;
        if (el.type === 'checkbox') {
          updates[key] = el.checked ? 1 : 0;
        } else if (key === 'fontFamily' && typeof val === 'string' && val.startsWith('sd:')) {
          // CrumBLE 4.1.1: SD-card font selection. Route the picked
          // family name to sdFontFamilyName; keep fontFamily at its
          // current built-in value so the device's font-resolution
          // path knows to consult the SD registry.
          updates.sdFontFamilyName = val.substring(3);
        } else if (key === 'fontFamily' && typeof val === 'string' && val.startsWith('builtin:')) {
          // Built-in font selection. Clear any prior SD selection so
          // the device falls back to the numeric fontFamily enum.
          updates.fontFamily = Number(val.substring(8));
          updates.sdFontFamilyName = '';
        } else {
          updates[key] = Number(val);
        }
      });
      confirm.disabled = true;
      confirm.textContent = 'Computing layout...';
      try {
        const resp = await fetch('/api/save-reader-settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(updates),
        });
        if (!resp.ok) {
          const errText = await resp.text();
          throw new Error('Preview failed: ' + errText);
        }
        const overridden = await resp.json();
        // CrumBLE 4.5.5 follow-up: the dry-run path skips the SD-font
        // ensureLoaded (loading a brand-new .cpfont per modal click was
        // a ~5KB MaxAlloc tax we didn't want to charge), so its
        // sdFontPickedPointSize echoes whatever font happens to be
        // resident -- stale when the user just picked a different SD
        // font. Override here from family.sizes the same way the old
        // save+refetch branch did. CLI consumes this to know which
        // <Family>_<pt>.cpfont to ship to WASM; without it the bake
        // would fall back to a guess and the device-side fontId
        // fingerprint wouldn't match the prepared layout.
        const familyVal = familySel.value;
        const sizeVal = sizeSel.value;
        if (familyVal.startsWith('sd:')) {
          const sdName = familyVal.substring(3);
          const family = SD_FONT_FAMILIES[sdName];
          if (family && family.sizes.length > 0) {
            const sorted = family.sizes.slice().sort((a, b) => (a | 0) - (b | 0));
            const idx = Math.min(sorted.length - 1, Math.max(0, sizeVal | 0));
            overridden.sdFontPickedPointSize = sorted[idx];
          }
        } else {
          // Built-in font: no SD picked-size to derive. Force 0 so the
          // downstream SD-font fetch path in prebakeChapters skips
          // entirely (its trigger is sdFontFamilyName non-empty).
          overridden.sdFontPickedPointSize = 0;
        }
        // Preserve any fields the dryRun response didn't carry (older
        // firmware contract is /api/reader-render-info; new dryRun
        // response should be a superset, but be defensive about
        // forward-compat).
        const merged = Object.assign({}, renderInfo, overridden);
        close();
        resolve(merged);
      } catch (e) {
        confirm.disabled = false;
        confirm.textContent = 'Looks good, optimize';
        alert('Could not compute layout preview: ' + (e.message || e));
      }
    };
  });
}

// CrumBLE 4.5.5+: secondary modal between preflight and prebake. Fires
// ONLY when the user landed on an SD font AND the local IndexedDB cache
// doesn't already have its bytes. Built-in font selections (Bitter,
// Lexend Deca, CharE-Ink) skip this entirely.
//
// Three outcomes:
//   - 'import' + bytes: user picked a .cpfont that matches the device's
//     fontId (verified via JS replication of the device's FNV hash);
//     caller writes the bytes to IDB and proceeds with the cached path
//   - 'skip': user chose to let the device send the font over WiFi
//     (~30s); the existing fetch path runs and caches afterward
//   - 'back': user wants to re-open the preflight (e.g. realized they
//     picked the wrong font); the caller loops back to preflight
//
// Cancellation (overlay click, Escape) rejects the promise to match the
// preflight modal's contract -- caller's existing catch handles abort.
function showFontImportModal({ family, pt, expectedFileSize, expectedFontId }) {
  return new Promise((resolve, reject) => {
    const overlay = document.createElement('div');
    overlay.style.cssText =
      'position:fixed;inset:0;background:rgba(0,0,0,0.5);display:flex;align-items:center;' +
      'justify-content:center;z-index:10000;font-family:-apple-system,system-ui,sans-serif;';
    const dialog = document.createElement('div');
    dialog.style.cssText =
      'background:#fff;color:#222;border-radius:12px;max-width:520px;width:90%;' +
      'padding:24px;box-shadow:0 12px 32px rgba(0,0,0,0.2);';

    const sizeFmt = expectedFileSize >= 1048576
      ? `${(expectedFileSize / 1048576).toFixed(1)} MB`
      : `${(expectedFileSize / 1024).toFixed(0)} KB`;

    const header = document.createElement('div');
    header.innerHTML =
      '<h2 style="margin:0 0 6px;font-size:20px;color:#222">Speed up this prebake?</h2>' +
      `<p style="margin:0 0 8px;color:#555;font-size:14px;line-height:1.5">Your SD font ` +
      `<strong>${family}_${pt}.cpfont</strong> (${sizeFmt}) isn't cached locally yet.</p>` +
      `<p style="margin:0 0 12px;color:#555;font-size:14px;line-height:1.5">Pick the same ` +
      `.cpfont file from your computer to skip a ~30 second WiFi download. ` +
      `Optional — you can also let the device send the font.</p>`;
    dialog.appendChild(header);

    const picker = document.createElement('input');
    picker.type = 'file';
    picker.accept = '.cpfont';
    picker.style.cssText = 'display:block;margin:8px 0 12px;font-size:14px;';
    dialog.appendChild(picker);

    const statusEl = document.createElement('p');
    statusEl.style.cssText = 'margin:6px 0;font-size:13px;color:#666;min-height:1.4em;';
    dialog.appendChild(statusEl);

    const btnRow = document.createElement('div');
    btnRow.style.cssText = 'display:flex;gap:8px;justify-content:flex-end;margin-top:16px;';
    const mkBtn = (label, primary, disabled) => {
      const b = document.createElement('button');
      b.textContent = label;
      b.disabled = !!disabled;
      b.style.cssText = 'padding:8px 16px;border-radius:6px;border:1px solid #ddd;cursor:pointer;font-size:14px;' +
        (primary ? 'background:#007AFF;color:#fff;border-color:#007AFF;' : 'background:#fff;color:#333;') +
        (disabled ? 'opacity:0.5;cursor:not-allowed;' : '');
      return b;
    };
    const backBtn = mkBtn('Back', false, false);
    const skipBtn = mkBtn('Skip — fetch from device', false, false);
    const confirmBtn = mkBtn('Import', true, true);
    btnRow.appendChild(backBtn);
    btnRow.appendChild(skipBtn);
    btnRow.appendChild(confirmBtn);
    dialog.appendChild(btnRow);

    overlay.appendChild(dialog);
    document.body.appendChild(overlay);

    let pickedBytes = null;
    const close = () => { try { document.body.removeChild(overlay); } catch (_) {} };

    // Inline style for the disabled primary button. Re-applied on every
    // failed-validation early-return so a previously-validated pick
    // (which switched to the enabled style) is properly dimmed again
    // when the user swaps in a wrong file.
    const disabledPrimaryStyle =
        'padding:8px 16px;border-radius:6px;border:1px solid #007AFF;cursor:not-allowed;font-size:14px;' +
        'background:#007AFF;color:#fff;opacity:0.5;';
    picker.onchange = async () => {
      const file = picker.files && picker.files[0];
      if (!file) {
        confirmBtn.disabled = true;
        confirmBtn.style.cssText = disabledPrimaryStyle;
        pickedBytes = null;
        statusEl.textContent = '';
        return;
      }
      confirmBtn.disabled = true;
      confirmBtn.style.cssText = disabledPrimaryStyle;
      pickedBytes = null;
      statusEl.style.color = '#666';
      statusEl.textContent = 'Validating...';
      if (file.size !== expectedFileSize) {
        statusEl.style.color = '#c00';
        statusEl.textContent =
          `Size mismatch: file is ${file.size.toLocaleString()} bytes; device's font is ` +
          `${expectedFileSize.toLocaleString()} bytes. Looks like a different build of the same family.`;
        return;
      }
      let bytes;
      try { bytes = new Uint8Array(await file.arrayBuffer()); }
      catch (e) {
        statusEl.style.color = '#c00';
        statusEl.textContent = `Could not read file: ${e.message || e}`;
        return;
      }
      const contentHash = computeCpFontContentHash(bytes);
      if (contentHash === null) {
        statusEl.style.color = '#c00';
        statusEl.textContent = 'Not a valid .cpfont (header parse failed).';
        return;
      }
      const computedId = computeFontIdJS(contentHash, family, pt);
      if (computedId !== expectedFontId) {
        statusEl.style.color = '#c00';
        statusEl.textContent =
          `Different font version: this file's fontId would be ${computedId}, but the device's font is ${expectedFontId}. ` +
          `Pick the .cpfont that's currently on the device's SD card.`;
        return;
      }
      statusEl.style.color = '#080';
      statusEl.textContent = `Valid — fontId ${computedId} matches device. Ready to import.`;
      pickedBytes = bytes;
      // CrumBLE 4.5.5+: when enabling, also strip the disabled-state CSS
      // (opacity:0.5 + cursor:not-allowed) that was baked into the inline
      // style at button-creation time. The `disabled` property alone
      // doesn't update inline cursor/opacity, so without this the
      // user sees the "no-drop" hover cursor on an actively-clickable
      // button -- looks like the validate step lied. Restore the same
      // primary-button style mkBtn(true, false) would have produced.
      confirmBtn.disabled = false;
      confirmBtn.style.cssText =
          'padding:8px 16px;border-radius:6px;border:1px solid #007AFF;cursor:pointer;font-size:14px;' +
          'background:#007AFF;color:#fff;';
    };

    backBtn.onclick = () => { close(); resolve({ action: 'back' }); };
    skipBtn.onclick = () => { close(); resolve({ action: 'skip' }); };
    confirmBtn.onclick = () => {
      if (!pickedBytes) return;
      close();
      resolve({ action: 'import', bytes: pickedBytes });
    };

    // Mousedown-on-overlay + mouseup-on-overlay = cancel (matches preflight modal pattern).
    let downOnOverlay = false;
    overlay.onmousedown = (e) => { downOnOverlay = (e.target === overlay); };
    overlay.onmouseup = (e) => {
      const wasOverlayClick = downOnOverlay && (e.target === overlay);
      downOnOverlay = false;
      if (wasOverlayClick) { close(); reject(new Error('Cancelled by user')); }
    };
  });
}

// CrumBLE 4.5.5+: wrapper around showOptimizerPreflightModal that adds
// the IDB cache check + font-import prompt step. Replaces direct calls
// to showOptimizerPreflightModal at the upload entry points so every
// bake path picks up the cache acceleration.
//
// Loops on a 'back' from the import modal so the user can rethink their
// font pick. Throws "Cancelled by user" up to the caller on any modal's
// overlay-click, matching the preflight modal's existing contract.
async function showPreflightAndFontImport(renderInfo, fileName) {
  while (true) {
    const merged = await showOptimizerPreflightModal(renderInfo, fileName);

    const fname = merged.sdFontFamilyName;
    const pt = merged.sdFontPickedPointSize | 0;
    // Built-in font (Bitter / Lexend / CharE-Ink) -- no SD font to cache, skip entirely.
    if (!fname || fname.length === 0 || pt <= 0) return merged;

    // Probe the device for the font's size + mtime + fontId. If the
    // endpoint is unavailable (older firmware mid-rollout), proceed
    // without prompt -- the in-prebake fetch path will still work, just
    // without the accelerator.
    let fontVer = null;
    try {
      const verResp = await fetch(
        `/api/fonts/version?family=${encodeURIComponent(fname)}&size=${pt}`,
        { cache: 'no-store' });
      if (verResp.ok) fontVer = await verResp.json();
    } catch (_) {}
    if (!fontVer || typeof fontVer.mtime !== 'number' || typeof fontVer.fileSize !== 'number') {
      return merged;
    }

    // CrumBLE 4.5.5+: override merged.fontId with the file-derived one
    // from /api/fonts/version. The dryRun preflight path skips
    // ensureLoaded for SD fonts to save heap, so renderInfo.fontId
    // comes back as the DEFAULT-font ID even when an SD font is
    // configured -- baking against that produces wrong glyph metrics
    // ("Outside range" errors at the right margin, visible character
    // spacing gaps the user reported). /api/fonts/version reads the
    // .cpfont's header + style TOC directly and runs the same FNV
    // SdCardFontManager would, so its fontId matches what the device
    // computes when actually rendering. Overriding here means the
    // WASM prebake's settings.json carries the correct id, the bake
    // stamps positions with the right metrics, and the device's
    // section-load fingerprint check passes on first open.
    if (typeof fontVer.fontId === 'number' && fontVer.fontId !== 0
        && fontVer.fontId !== merged.fontId) {
      try {
        console.log(`[font-cache] overriding merged.fontId: ${merged.fontId} -> ${fontVer.fontId} (from /api/fonts/version)`);
      } catch (_) {}
      merged.fontId = fontVer.fontId | 0;
    }

    // Cache check. If hit, no UI needed.
    const fingerprint = `${fname}|${pt}|${fontVer.mtime}|${fontVer.fileSize}`;
    const cached = await fontCacheGet(fingerprint);
    if (cached) return merged;

    // Modal's verification target is the same file-derived fontId we
    // just overrode merged with. Fallback for older firmwares that
    // didn't ship the fontId field in /api/fonts/version.
    const expectedFontId = (typeof fontVer.fontId === 'number' && fontVer.fontId !== 0)
      ? (fontVer.fontId | 0)
      : (merged.fontId | 0);

    // Cache miss -- prompt the user.
    const result = await showFontImportModal({
      family: fname,
      pt,
      expectedFileSize: fontVer.fileSize,
      expectedFontId,
    });
    if (result.action === 'back') continue;  // re-show preflight
    if (result.action === 'import' && result.bytes) {
      await fontCachePut(fingerprint, fname, pt, fontVer.mtime, result.bytes);
      try { console.log(`[font-cache] imported from user: ${fingerprint}`); } catch (_) {}
    }
    // 'skip' or 'import' -- proceed with prebake.
    return merged;
  }
}

// =============================================================================
// CrumBLE Phase 5b: chapter-indexing prebake (WASM-backed)
// =============================================================================
//
// The "Optimize chapter indexing" checkbox runs prebakeChapters() AFTER the
// EPUB upload completes. It executes the prebake CLI (cross-compiled to WASM
// in tools/crumble-prebake) inside the browser against the just-uploaded
// EPUB. The WASM emits a per-book cache directory under MEMFS; we then
// upload each file individually via the file manager's /upload endpoint.
//
// Why no zip / no extractor: tonight's debugging showed that a single big
// upload-and-extract on the device blew its heap, and a "lazy" tick-paced
// extractor competed badly with the reader for the SD bus. Individual
// uploads through the proven /upload endpoint sidestep both problems --
// each request is a ~5-30 KB write, sequential, no extraction.

let crumblePrebakeModuleFactoryPromise = null;
let crumblePrebakeModulePromise = null;

// CrumBLE 4.7: the ~2.4 MB prebake .wasm is no longer embedded in firmware
// flash (it was the single largest blob and pushed the app image past the
// stock 6.25 MB slot). Resolution order:
//   0. localStorage 'crumbleWasmUrl' -- manual/debug override
//   1. device /js/crumble-prebake.wasm -- only debug builds embed it now
//   2. IndexedDB -- bytes cached by a previous CDN fetch (offline/AP mode)
//   3. jsDelivr pinned to the running firmware's release tag, falling back
//      to @main for dev builds whose tag doesn't exist yet
// A successful CDN fetch is written back to IndexedDB keyed by firmware
// version so later AP-mode sessions work offline. Note IDB is per-origin
// (the device's IP), so a changed device IP means one more online fetch.
const WASM_CACHE_DB_NAME = 'crumble-wasm-cache';
const WASM_CACHE_STORE = 'wasm';
let prebakeWasmPromise = null;

function openWasmCacheDb() {
  if (typeof indexedDB === 'undefined') return Promise.reject(new Error('no indexedDB'));
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(WASM_CACHE_DB_NAME, 1);
    req.onupgradeneeded = () => {
      if (!req.result.objectStoreNames.contains(WASM_CACHE_STORE)) {
        req.result.createObjectStore(WASM_CACHE_STORE);
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error || new Error('idb open failed'));
  });
}

function wasmCacheGet(key) {
  return openWasmCacheDb().then((db) => new Promise((resolve) => {
    const tx = db.transaction(WASM_CACHE_STORE, 'readonly');
    const req = tx.objectStore(WASM_CACHE_STORE).get(key);
    req.onsuccess = () => { resolve(req.result || null); db.close(); };
    req.onerror = () => { resolve(null); db.close(); };
  })).catch(() => null);
}

function wasmCachePut(key, bytes) {
  return openWasmCacheDb().then((db) => new Promise((resolve) => {
    const tx = db.transaction(WASM_CACHE_STORE, 'readwrite');
    tx.objectStore(WASM_CACHE_STORE).put(bytes, key);
    tx.oncomplete = () => { resolve(true); db.close(); };
    tx.onerror = () => { resolve(false); db.close(); };
    tx.onabort = () => { resolve(false); db.close(); };
  })).catch(() => false);
}

async function getFirmwareVersionForWasm() {
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    const j = await r.json();
    const m = /CrumBLE (\d+\.\d+\.\d+)/.exec(j.version || '');
    if (m) return m[1];
  } catch (e) { /* fall through */ }
  return null;
}

function wasmBlobUrl(bytes) {
  // application/wasm type so instantiateStreaming stays on the fast path.
  return URL.createObjectURL(new Blob([bytes], { type: 'application/wasm' }));
}

// Returns {url, identity}. url feeds emscripten's locateFile; identity is a
// stable string naming the wasm build, used in the bake fingerprint.
function resolvePrebakeWasm() {
  if (prebakeWasmPromise) return prebakeWasmPromise;
  prebakeWasmPromise = (async () => {
    const override = (() => {
      try { return localStorage.getItem('crumbleWasmUrl'); } catch (e) { return null; }
    })();
    if (override) return { url: override, identity: 'override' };

    // 1. Device route -- 404s unless the firmware was built with
    //    CRUMBLE_EMBED_WASM=1. Keeps debug builds and old firmware working.
    try {
      const head = await fetch('/js/crumble-prebake.wasm', { method: 'HEAD', cache: 'no-store' });
      if (head.ok) {
        return {
          url: location.origin + '/js/crumble-prebake.wasm',
          identity: head.headers.get('etag') || 'device',
        };
      }
    } catch (e) { /* device route unavailable -- keep going */ }

    const ver = await getFirmwareVersionForWasm();
    const cacheKey = 'wasm-' + (ver || 'unknown');

    // 2. IndexedDB bytes from a previous CDN fetch.
    const cached = await wasmCacheGet(cacheKey);
    if (cached) return { url: wasmBlobUrl(cached), identity: 'idb-' + (ver || 'unknown') };

    // 3. jsDelivr. Tag first (immutable, matches this firmware); @main
    //    covers dev builds between releases.
    const candidates = [];
    if (ver) candidates.push({ ref: 'crumble-v' + ver, id: 'cdn-' + ver });
    candidates.push({ ref: 'main', id: 'cdn-main' });
    for (const c of candidates) {
      try {
        const resp = await fetch(
          'https://cdn.jsdelivr.net/gh/imshentastic/CrumBLE@' + c.ref + '/web-assets/crumble-prebake.wasm',
          { cache: 'force-cache' });
        if (!resp.ok) continue;
        const bytes = await resp.arrayBuffer();
        await wasmCachePut(cacheKey, bytes);  // best-effort
        return { url: wasmBlobUrl(bytes), identity: c.id };
      } catch (e) { /* try next candidate */ }
    }

    prebakeWasmPromise = null;  // allow retry once the network is back
    throw new Error(
      'Optimizer engine unavailable: this firmware does not embed it and it ' +
      'could not be downloaded. Connect this browser to the internet once ' +
      'and retry -- it is then cached for offline use.');
  })();
  return prebakeWasmPromise;
}

// CrumBLE 4.5.4 task #5C continuation: Web Worker host for the WASM
// prebake. The user-facing problem this solves: callMain() blocks the
// JS main thread for 5-10 minutes on Harry Potter-scale CJK books, so
// the progress bar text freezes and looks like the device crashed.
// CSS shimmer keeps moving (compositor thread) but it's too subtle.
//
// Worker isolation lets the WASM run off the main thread; the worker
// posts a 'stdout' message per print() line, and the main thread
// receives + heartbeat-updates the UI smoothly even when WASM is mid
// section-layout. Same Module instance lives entirely in the worker --
// FS, HEAP, callMain. Main thread never touches Module directly.
//
// Inline Blob worker (no separate firmware file) -- workerSource is a
// JS string, URL.createObjectURL(new Blob([source])) is the URL we feed
// to new Worker(). importScripts inside the worker pulls the existing
// /js/crumble-prebake.js factory (already built with
// ENVIRONMENT=web,worker,node), so no build change required.
const PREBAKE_WORKER_SOURCE = `
  let Module = null;
  function emit(type, payload, transfer) {
    self.postMessage({ type, ...(payload || {}) }, transfer || []);
  }
  self.onmessage = async (e) => {
    const msg = e.data;
    try {
      if (msg.cmd === 'init') {
        // factoryUrl is an absolute same-origin URL (location.origin + path)
        // computed on the main thread. importScripts here loads the
        // Emscripten-emitted JS, which sets self.CrumblePrebake = factory.
        importScripts(msg.factoryUrl);
        Module = await self.CrumblePrebake({
          print: (line) => emit('stdout', { line }),
          printErr: (line) => emit('stderr', { line }),
          // The factory needs to know where to fetch the .wasm. The main
          // thread resolves it (device/IDB/CDN -- see resolvePrebakeWasm)
          // and passes the final URL in; everything else stays same-dir
          // as the .js.
          locateFile: (name) => (name.endsWith('.wasm') && msg.wasmUrl)
              ? msg.wasmUrl
              : msg.factoryUrl.replace(/\\/[^/]+$/, '/' + name),
        });
        emit('ready', {});
        return;
      }
      if (msg.cmd === 'run') {
        if (!Module) { emit('error', { message: 'worker not initialized' }); return; }
        // Clean any leftover MEMFS state from a previous run. EXIT_RUNTIME=0
        // means the module is reused, so /input.epub etc. may still exist.
        const tryUnlink = (p) => { try { Module.FS.unlink(p); } catch (e) {} };
        tryUnlink('/input.epub');
        tryUnlink('/settings.json');
        tryUnlink('/sd_font.cpfont');
        // Recursive rmdir for /out: walk and delete.
        function rmrf(path) {
          try {
            const st = Module.FS.stat(path);
            if (Module.FS.isDir(st.mode)) {
              for (const entry of Module.FS.readdir(path)) {
                if (entry === '.' || entry === '..') continue;
                rmrf(path + '/' + entry);
              }
              try { Module.FS.rmdir(path); } catch (e) {}
            } else {
              try { Module.FS.unlink(path); } catch (e) {}
            }
          } catch (e) {}
        }
        rmrf('/out');
        // Stage inputs. epubBytes / sdFontBytes arrive as transferred
        // ArrayBuffers; wrap in Uint8Array for writeFile.
        Module.FS.writeFile('/input.epub', new Uint8Array(msg.epubBytes));
        Module.FS.writeFile('/settings.json', msg.settingsJson);
        if (msg.sdFontBytes) {
          Module.FS.writeFile('/sd_font.cpfont', new Uint8Array(msg.sdFontBytes));
        }
        try { Module.FS.mkdir('/out'); } catch (e) {}
        const rc = Module.callMain(msg.cliArgs);
        if (rc !== 0) {
          emit('error', { message: 'CLI exited with code ' + rc, rc });
          return;
        }
        // Walk /out/.crosspoint to find the epub_<hash> cache dir +
        // collect all files as transferable ArrayBuffers.
        const crosspointDir = '/out/.crosspoint';
        const roots = Module.FS.readdir(crosspointDir).filter(
          (n) => n.startsWith('epub_'));
        if (roots.length !== 1) {
          emit('error', { message: 'expected one epub_* cache dir, found ' + roots.length });
          return;
        }
        const hashId = roots[0];
        const memCacheDir = crosspointDir + '/' + hashId;
        const files = [];
        const transferables = [];
        function walk(dir, relPrefix) {
          for (const entry of Module.FS.readdir(dir)) {
            if (entry === '.' || entry === '..') continue;
            const full = dir + '/' + entry;
            const st = Module.FS.stat(full);
            const rel = relPrefix ? relPrefix + '/' + entry : entry;
            if (Module.FS.isDir(st.mode)) {
              walk(full, rel);
            } else {
              const u8 = Module.FS.readFile(full);
              // readFile returns a Uint8Array view; slice into a fresh
              // ArrayBuffer so we can transfer it (zero-copy) to main.
              const buf = u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength);
              files.push({ relPath: rel, buffer: buf, size: u8.byteLength });
              transferables.push(buf);
            }
          }
        }
        walk(memCacheDir, '');
        emit('done', { hashId, files }, transferables);
        return;
      }
    } catch (err) {
      emit('error', { message: (err && err.message) || String(err) });
    }
  };
`;

let prebakeWorker = null;
let prebakeWorkerReadyPromise = null;

// Lazy-create the prebake worker. Reused across multiple book bakes in
// the same session -- the WASM module is heavy to load (~3-5 s for the
// ~1.3 MB wasm decompress + instantiate), so paying that once per
// session beats per-book. The worker outlives a single prebakeChapters()
// call but is torn down by terminatePrebakeWorker() on user cancel.
async function ensurePrebakeWorker() {
  if (prebakeWorker && prebakeWorkerReadyPromise) {
    await prebakeWorkerReadyPromise;
    return prebakeWorker;
  }
  // Resolve the wasm source before spinning up the worker so a fetch
  // failure surfaces as its own message, not a generic worker-init error.
  const wasm = await resolvePrebakeWasm();
  const blob = new Blob([PREBAKE_WORKER_SOURCE], { type: 'application/javascript' });
  const worker = new Worker(URL.createObjectURL(blob));
  prebakeWorker = worker;
  prebakeWorkerReadyPromise = new Promise((resolve, reject) => {
    const onMsg = (e) => {
      if (e.data && e.data.type === 'ready') {
        worker.removeEventListener('message', onMsg);
        worker.removeEventListener('error', onErr);
        resolve();
      } else if (e.data && e.data.type === 'error') {
        worker.removeEventListener('message', onMsg);
        worker.removeEventListener('error', onErr);
        reject(new Error(e.data.message || 'prebake worker init failed'));
      }
    };
    const onErr = (err) => {
      worker.removeEventListener('message', onMsg);
      worker.removeEventListener('error', onErr);
      reject(new Error('prebake worker error: ' + (err.message || err.type || 'unknown')));
    };
    worker.addEventListener('message', onMsg);
    worker.addEventListener('error', onErr);
    worker.postMessage({
      cmd: 'init',
      factoryUrl: location.origin + '/js/crumble-prebake.js',
      wasmUrl: wasm.url,
    });
  });
  await prebakeWorkerReadyPromise;
  return worker;
}

function terminatePrebakeWorker() {
  if (prebakeWorker) {
    try { prebakeWorker.terminate(); } catch (e) {}
  }
  prebakeWorker = null;
  prebakeWorkerReadyPromise = null;
}
window.terminatePrebakeWorker = terminatePrebakeWorker;

// Lazily inject /js/crumble-prebake.js and grab the factory it exposes.
// Cached: subsequent calls return the same factory.
function loadCrumblePrebakeFactory() {
  if (crumblePrebakeModuleFactoryPromise) return crumblePrebakeModuleFactoryPromise;
  crumblePrebakeModuleFactoryPromise = new Promise((resolve, reject) => {
    if (typeof window.CrumblePrebake === 'function') {
      resolve(window.CrumblePrebake);
      return;
    }
    const s = document.createElement('script');
    s.src = '/js/crumble-prebake.js';
    s.onload = () => {
      if (typeof window.CrumblePrebake !== 'function') {
        reject(new Error('crumble-prebake.js loaded but CrumblePrebake factory missing'));
        return;
      }
      resolve(window.CrumblePrebake);
    };
    s.onerror = () => reject(new Error('Failed to load /js/crumble-prebake.js (firmware may not include the WASM module)'));
    document.head.appendChild(s);
  });
  return crumblePrebakeModuleFactoryPromise;
}

// Buffer for capturing the CLI's print/printErr output across a single
// callMain. Cleared before each run; on non-zero exit we include the
// captured contents in the thrown error so the user sees WHAT the CLI
// rejected instead of just an opaque "exit code N".
const crumblePrebakeOutputBuf = [];

// CrumBLE 4.5.4: live progress heartbeat. Set by prebakeChapters() before
// Module.callMain() and cleared after. The print/printErr hooks invoke it
// on every WASM stdout line so the UI bar moves during the long CLI run.
// Defined at module scope (not inside the factory) so the same function
// instance is hooked once at module-load and read per-call.
let crumblePrebakeHeartbeat = null;

// Get a ready Module instance. The factory is invoked once and the resulting
// Module is reused across runs (EXIT_RUNTIME=0 keeps MEMFS + libc alive
// between callMain invocations). This means we pay the ~850 KB WASM
// download + instantiation cost once per session, not per book.
async function loadCrumblePrebakeModule() {
  if (crumblePrebakeModulePromise) return crumblePrebakeModulePromise;
  crumblePrebakeModulePromise = (async () => {
    const wasm = await resolvePrebakeWasm();
    const factory = await loadCrumblePrebakeFactory();
    return await factory({
      locateFile: (name) => name.endsWith('.wasm') ? wasm.url : '/js/' + name,
      // Route emscripten stdout/stderr through three places:
      //   1. The optimizer log panel (per-line, prefixed by [prebake] /
      //      [prebake-err]) -- visible while the modal is open.
      //   2. console.log / console.error -- persists across page reloads
      //      when DevTools "Preserve log" is on.
      //   3. crumblePrebakeOutputBuf -- the running buffer we attach to
      //      a thrown error when callMain returns non-zero, so the user
      //      sees the CLI's own failure message even after the log
      //      panel scrolls or the modal closes.
      print: (msg) => {
        crumblePrebakeOutputBuf.push(msg);
        try { console.log('[prebake]', msg); } catch (e) {}
        try { log(`[prebake] ${msg}`, '', 'PRE'); } catch (e) {}
        // CrumBLE 4.5.4: live heartbeat. callMain() blocks the JS main
        // thread, so setInterval/setTimeout in JS can't tick during the
        // CLI run. But Module.print fires sync on every stdout line --
        // hook it to nudge the progress bar so 80->99% actually moves
        // instead of sticking at the "started prebake" milestone forever.
        // Caller sets crumblePrebakeHeartbeat to (lineText) => void before
        // callMain and clears it after.
        if (typeof crumblePrebakeHeartbeat === 'function') {
          try { crumblePrebakeHeartbeat(msg); } catch (e) {}
        }
      },
      printErr: (msg) => {
        crumblePrebakeOutputBuf.push(msg);
        try { console.error('[prebake-err]', msg); } catch (e) {}
        try { log(`[prebake-err] ${msg}`, 'warning', 'PRE-ERR'); } catch (e) {}
        if (typeof crumblePrebakeHeartbeat === 'function') {
          try { crumblePrebakeHeartbeat(msg); } catch (e) {}
        }
      },
    });
  })();
  return crumblePrebakeModulePromise;
}

// Upload a single byte buffer to /upload?path=<destDir> with filename <name>.
// Returns the HTTP status code; throws on network failure.
// CrumBLE 4.5.5+: bake-fingerprint helpers. The pack-upload resume check
// compares EXISTING device files to the about-to-be-uploaded set by SIZE
// ONLY -- which is safe when WASM output is byte-deterministic from inputs,
// but breaks when inputs change without sizes changing (e.g. user iterated
// firmware that produced different content at coincidentally identical
// byte lengths, then resume falsely matched those stale files and the
// device couldn't open the book). The fingerprint stamps each successful
// bake with a tiny marker file whose name encodes the bake's inputs. On
// the next bake, the browser computes its own fingerprint, looks for a
// matching marker in the listing -- if present, size-match resume is
// trustworthy; if absent or different, every existing file is treated as
// stale and re-uploaded. ~30 ms total cost per optimize.

async function sha256hex(input) {
  const data = typeof input === 'string' ? new TextEncoder().encode(input) : input;
  // CrumBLE 4.5.5+: prefer Web Crypto when available, fall back when not.
  // The device serves the optimizer at http://<lan-ip>/ which is NOT a
  // Secure Context per W3C -- so window.crypto.subtle is undefined and any
  // .digest access throws "Cannot read properties of undefined". Field
  // log: every prebake on the device's captive portal hit
  // "Chapter prebake failed: Cannot read properties of undefined
  // (reading 'digest')" the moment the bake-fingerprint marker logic
  // started calling sha256hex. The fingerprint marker only needs
  // *uniqueness* per bake-inputs, not cryptographic security, so a
  // non-crypto deterministic hash is a fine substitute on the insecure
  // origin. Returns the same 64-hex-char shape so downstream slicers
  // (`fpFull.slice(0, 16)`) keep working unchanged.
  if (typeof crypto !== 'undefined' && crypto.subtle && typeof crypto.subtle.digest === 'function') {
    const hash = await crypto.subtle.digest('SHA-256', data);
    return Array.from(new Uint8Array(hash))
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('');
  }
  // cyrb53 double-hash, run 4x with different seeds to fill 64 hex chars.
  // ~50 ns/byte; negligible vs SHA-256 (~5 ns/byte native but with API
  // overhead for short inputs). Used for fingerprint-marker filenames
  // only -- never for anything security-sensitive.
  function cyrb53(buf, seed) {
    let h1 = 0xdeadbeef ^ seed;
    let h2 = 0x41c6ce57 ^ seed;
    for (let i = 0; i < buf.length; i++) {
      const ch = buf[i];
      h1 = Math.imul(h1 ^ ch, 2654435761);
      h2 = Math.imul(h2 ^ ch, 1597334677);
    }
    h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507);
    h1 ^= Math.imul(h2 ^ (h2 >>> 13), 3266489909);
    h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507);
    h2 ^= Math.imul(h1 ^ (h1 >>> 13), 3266489909);
    return (((h2 & 0xffffffff) >>> 0).toString(16).padStart(8, '0')
          + ((h1 & 0xffffffff) >>> 0).toString(16).padStart(8, '0'));
  }
  return cyrb53(data, 0xa1)
       + cyrb53(data, 0xb2)
       + cyrb53(data, 0xc3)
       + cyrb53(data, 0xd4);
}

// CrumBLE 4.5.5+: IndexedDB cache for the multi-MB SD .cpfont bytes.
// First fetch of a (family, pt) pair pays the full 30-40s WiFi transfer
// from the device; every subsequent prebake that wants the same font
// gets a cache hit via the cheap /api/fonts/version probe (~50ms round
// trip returning size + FAT mtime) and skips the big GET entirely.
//
// Cache key: `${family}|${pt}|${mtime}|${fileSize}` -- the family+pt
// identify the font; mtime+fileSize invalidate when the user replaces
// the font file on the device (a re-upload bumps the FAT mtime). No
// deviceUuid in the key: two devices that legitimately share the same
// font bytes can share cache entries (the bytes ARE the same).
//
// Storage: ArrayBuffer per entry, lastUsed Date.now() for LRU eviction
// above 50 MB total. Silent degradation: if indexedDB is unavailable
// (private mode, very old browser), all calls return null/skip and the
// caller falls through to the existing streaming fetch path.

const FONT_CACHE_DB_NAME = 'crumble-font-cache-v1';
const FONT_CACHE_STORE = 'fontBytes';
const FONT_CACHE_BUDGET_BYTES = 50 * 1024 * 1024;

function openFontCacheDb() {
  if (typeof indexedDB === 'undefined') return Promise.reject(new Error('no indexedDB'));
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(FONT_CACHE_DB_NAME, 1);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(FONT_CACHE_STORE)) {
        const store = db.createObjectStore(FONT_CACHE_STORE, { keyPath: 'fingerprint' });
        store.createIndex('lastUsed', 'lastUsed');
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function fontCacheGet(fingerprint) {
  try {
    const db = await openFontCacheDb();
    try {
      return await new Promise((resolve, reject) => {
        const tx = db.transaction(FONT_CACHE_STORE, 'readwrite');
        const store = tx.objectStore(FONT_CACHE_STORE);
        const req = store.get(fingerprint);
        req.onsuccess = () => {
          const entry = req.result;
          if (!entry) { resolve(null); return; }
          // Bump lastUsed so this entry survives the next eviction pass.
          entry.lastUsed = Date.now();
          store.put(entry);
          resolve(entry.bytes);
        };
        req.onerror = () => reject(req.error);
      });
    } finally { db.close(); }
  } catch (e) { return null; }
}

async function fontCachePut(fingerprint, family, pt, mtime, bytes) {
  try {
    const db = await openFontCacheDb();
    try {
      // Store as ArrayBuffer (not Uint8Array view) -- IDB clones either, but
      // the buffer form avoids a wasted view object on retrieval.
      const buf = bytes.buffer.byteLength === bytes.byteLength
        ? bytes.buffer
        : bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      await new Promise((resolve, reject) => {
        const tx = db.transaction(FONT_CACHE_STORE, 'readwrite');
        const store = tx.objectStore(FONT_CACHE_STORE);
        store.put({
          fingerprint, family, pt, mtime,
          bytes: buf,
          size: buf.byteLength,
          lastUsed: Date.now(),
        });
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
        tx.onabort = () => reject(tx.error || new Error('tx aborted'));
      });
    } finally { db.close(); }
  } catch (e) { /* silent -- caching is best-effort */ }
}

async function fontCacheEvictLRU(budgetBytes) {
  try {
    const db = await openFontCacheDb();
    try {
      await new Promise((resolve) => {
        const tx = db.transaction(FONT_CACHE_STORE, 'readwrite');
        const store = tx.objectStore(FONT_CACHE_STORE);
        const idx = store.index('lastUsed');
        const entries = [];
        let totalBytes = 0;
        idx.openCursor().onsuccess = (e) => {
          const cursor = e.target.result;
          if (cursor) {
            entries.push({ key: cursor.primaryKey, size: cursor.value.size | 0 });
            totalBytes += cursor.value.size | 0;
            cursor.continue();
          } else {
            // Cursor walked ascending by lastUsed; oldest first.
            let removed = 0;
            let i = 0;
            while (totalBytes > budgetBytes && i < entries.length) {
              store.delete(entries[i].key);
              totalBytes -= entries[i].size;
              removed++;
              i++;
            }
            if (removed > 0) {
              try { console.log(`[font-cache] evicted ${removed} entr(ies) to fit ${budgetBytes} budget`); } catch (_) {}
            }
            resolve();
          }
        };
      });
    } finally { db.close(); }
  } catch (e) { /* silent */ }
}

// CrumBLE 4.5.5+: JS replication of the device-side fontId computation so
// the browser can verify a user-picked .cpfont matches the device's
// currently-selected SD font BEFORE writing it to the cache. Algorithm
// (mirrors SdCardFont.cpp + SdCardFontManager.cpp):
//
//   1. contentHash = FNV-1a over:
//        - first 32 bytes (file header)
//        - then for each style: 32-byte TOC entry (styleCount is at byte 12
//          of the header; TOC entries follow immediately after the header)
//   2. fontId = continued FNV-1a from contentHash, hashing in:
//        - family-name bytes (ASCII)
//        - one byte: pointSize
//      then cast to int32; if result is 0, use 1 (device reserves 0 as
//      "not found" sentinel)
//
// If the result matches `renderInfo.fontId` the device returned, the
// user picked the EXACT same .cpfont the device has on SD. Filesize
// match alone would catch obviously-wrong picks but miss the case where
// a user has two builds of the same family at the same point size with
// coincidentally identical byte counts. The hash check is bulletproof.

const FNV_OFFSET_32 = 2166136261;  // 0x811C9DC5
const FNV_PRIME_32 = 16777619;     // 0x01000193
const CPFONT_HEADER_SIZE = 32;
const CPFONT_STYLE_TOC_ENTRY_SIZE = 32;

function computeCpFontContentHash(bytes) {
  if (bytes.length < CPFONT_HEADER_SIZE + CPFONT_STYLE_TOC_ENTRY_SIZE) return null;
  let hash = FNV_OFFSET_32 >>> 0;
  // Header: first 32 bytes
  for (let i = 0; i < CPFONT_HEADER_SIZE; i++) {
    hash = (hash ^ bytes[i]) >>> 0;
    hash = Math.imul(hash, FNV_PRIME_32) >>> 0;
  }
  // styleCount is at byte 12 of the header
  const styleCount = bytes[12];
  if (styleCount === 0 || styleCount > 16) return null;  // device's MAX_STYLES is 16
  const minLen = CPFONT_HEADER_SIZE + styleCount * CPFONT_STYLE_TOC_ENTRY_SIZE;
  if (bytes.length < minLen) return null;
  let offset = CPFONT_HEADER_SIZE;
  for (let s = 0; s < styleCount; s++) {
    for (let i = 0; i < CPFONT_STYLE_TOC_ENTRY_SIZE; i++) {
      hash = (hash ^ bytes[offset + i]) >>> 0;
      hash = Math.imul(hash, FNV_PRIME_32) >>> 0;
    }
    offset += CPFONT_STYLE_TOC_ENTRY_SIZE;
  }
  return hash >>> 0;
}

function computeFontIdJS(contentHash, family, pt) {
  let hash = contentHash >>> 0;
  for (let i = 0; i < family.length; i++) {
    hash = (hash ^ (family.charCodeAt(i) & 0xff)) >>> 0;
    hash = Math.imul(hash, FNV_PRIME_32) >>> 0;
  }
  hash = (hash ^ (pt & 0xff)) >>> 0;
  hash = Math.imul(hash, FNV_PRIME_32) >>> 0;
  // Cast to int32 the way C++ does: sign-extend the top bit.
  const id = hash | 0;
  return id !== 0 ? id : 1;
}

// Combines the bake-relevant inputs into a stable 16-hex-char fingerprint.
// Returning a truncated hash keeps marker filenames short while leaving
// ~64 bits of collision resistance -- way more than we need for a
// per-book-per-bake identity check.
async function computeBakeFingerprint(renderInfo, epubBytes) {
  // 1. WASM identity from the resolver (device etag, idb-<ver>, or
  //    cdn-<ver> -- see resolvePrebakeWasm). Field name stays wasmEtag so
  //    fingerprints from embedded-wasm firmwares keep their shape; the
  //    value changing across an upgrade just forces one re-bake, which is
  //    the safe direction.
  let wasmEtag = 'no-etag';
  try {
    wasmEtag = (await resolvePrebakeWasm()).identity;
  } catch (e) {
    // Resolver failed (offline, no cache) -- proceed with the best-effort
    // fingerprint; the actual bake will surface the real error.
  }

  // 2. Cheap content fingerprint for the EPUB: size + sha256 over the
  //    first 4 KB + last 4 KB of bytes. Avoids the cost of hashing a
  //    full 10 MB book on the main thread; the head/tail combo is good
  //    enough to detect "different book at same path" or "modified
  //    in-place" cases that share a byte count.
  const HEAD = Math.min(4096, epubBytes.length);
  const TAIL = Math.min(4096, Math.max(0, epubBytes.length - HEAD));
  const sample = new Uint8Array(HEAD + TAIL);
  sample.set(epubBytes.subarray(0, HEAD), 0);
  if (TAIL > 0) {
    sample.set(epubBytes.subarray(epubBytes.length - TAIL), HEAD);
  }
  const epubSampleHash = await sha256hex(sample);

  // 3. Canonicalize the inputs so the same logical bake always produces the
  //    same fingerprint. JSON.stringify is good enough here -- the renderInfo
  //    shape is fixed by /api/reader-render-info, so the key order is stable.
  const fpInputs = {
    wasmEtag,
    epubSize: epubBytes.length,
    epubSampleHash,
    settings: renderInfo,
  };
  const fpFull = await sha256hex(JSON.stringify(fpInputs));
  return fpFull.slice(0, 16);
}

// CrumBLE 4.5.5+: pack-mode cache uploader. Builds one binary stream
// containing a leading TOC + concatenated file payloads, sends it over a
// single WebSocket frame sequence, and waits for PACK_DONE. The server
// demuxes byte-by-byte without holding the full pack in RAM.
//
// Wire format (LE integers):
//   "CMBPACK1"            8 bytes magic
//   <fileCount>           u32
//   for each file:
//     <pathLen>           u32 (excludes any null terminator)
//     <pathBytes>         path string, no null terminator
//     <fileSize>          u32
//   then for each file in same order:
//     <fileBytes>         raw payload
//
// items: [{relPath, bytes, destDir, fname}, ...]
// Returns {ok: bool, error?: string, uploaded: count, totalBytes: number}
async function uploadCacheFilesAsPackWs(items, onProgress, isCancelled) {
  if (items.length === 0) return { ok: true, uploaded: 0, totalBytes: 0 };

  const WS_CHUNK_SIZE = 4096;
  // Build the TOC (header + per-file metadata) as a single Uint8Array. Path
  // strings get UTF-8 encoded first so multi-byte chars (uncommon in cache
  // paths, but cheap to handle) are counted correctly.
  const encoder = new TextEncoder();
  const pathEncoded = items.map((it) => {
    const full = it.destDir.endsWith('/') ? it.destDir + it.fname : `${it.destDir}/${it.fname}`;
    return encoder.encode(full);
  });
  let tocLen = 8 + 4;  // magic + count
  for (let i = 0; i < items.length; i++) tocLen += 4 + pathEncoded[i].length + 4;
  let payloadLen = 0;
  for (const it of items) payloadLen += it.bytes.length;
  const totalBytes = tocLen + payloadLen;
  // Hard cap matches the server-side kPackMaxTotalBytes (32 MB). Anything
  // larger should split into multiple packs at the caller.
  if (totalBytes > 32 * 1024 * 1024) {
    return { ok: false, error: 'pack exceeds 32 MB', uploaded: 0, totalBytes: 0 };
  }
  const toc = new Uint8Array(tocLen);
  const tocView = new DataView(toc.buffer);
  toc.set(encoder.encode('CMBPACK1'), 0);
  tocView.setUint32(8, items.length, true);
  let off = 12;
  for (let i = 0; i < items.length; i++) {
    const p = pathEncoded[i];
    tocView.setUint32(off, p.length, true); off += 4;
    toc.set(p, off); off += p.length;
    tocView.setUint32(off, items[i].bytes.length, true); off += 4;
  }

  // Open WS + simple FIFO queue for inbound messages.
  const queue = [];
  const waiters = [];
  let wsClosed = false;
  let closeReason = null;
  const ws = await new Promise((resolve, reject) => {
    const w = new WebSocket(getWsUrl());
    const timer = setTimeout(() => { try { w.close(); } catch (_) {} reject(new Error('WS open timeout')); }, 10000);
    w.onopen = () => { clearTimeout(timer); resolve(w); };
    w.onerror = () => { clearTimeout(timer); reject(new Error('WS open failed')); };
  });
  ws.onmessage = (evt) => {
    if (waiters.length > 0) {
      const w = waiters.shift();
      clearTimeout(w.timer);
      w.resolve(evt.data);
    } else {
      queue.push(evt.data);
    }
  };
  ws.onerror = () => { wsClosed = true; closeReason = 'error'; failAllWaiters(); };
  ws.onclose = () => { wsClosed = true; closeReason = 'closed'; failAllWaiters(); };
  function failAllWaiters() {
    while (waiters.length > 0) {
      const w = waiters.shift();
      clearTimeout(w.timer);
      w.reject(new Error(`WS ${closeReason}`));
    }
  }
  function readMsg(timeoutMs) {
    return new Promise((resolve, reject) => {
      if (queue.length > 0) { resolve(queue.shift()); return; }
      if (wsClosed) { reject(new Error(`WS ${closeReason}`)); return; }
      const timer = setTimeout(() => {
        const idx = waiters.findIndex(w => w.resolve === resolve);
        if (idx >= 0) waiters.splice(idx, 1);
        reject(new Error('WS message timeout'));
      }, timeoutMs);
      waiters.push({ resolve, reject, timer });
    });
  }

  try {
    ws.send(`PACK_START:${totalBytes}`);
    const ready = await readMsg(8000);
    if (ready !== 'PACK_READY') {
      if (typeof ready === 'string' && ready.startsWith('ERROR:')) {
        return { ok: false, error: ready.substring(6), uploaded: 0, totalBytes: 0 };
      }
      return { ok: false, error: `unexpected response: ${ready}`, uploaded: 0, totalBytes: 0 };
    }

    // Send the TOC in WS_CHUNK_SIZE-sized frames. Use the same bufferedAmount
    // throttle as the per-file path -- on ESP32-C3 the WS recv buffer is
    // tight and pumping too fast triggers a slow disconnect.
    const sendBuf = async (buf) => {
      let p = 0;
      while (p < buf.length) {
        if (isCancelled && isCancelled()) throw new Error('Cancelled by user');
        if (ws.readyState !== WebSocket.OPEN) throw new Error('WS closed during send');
        while (ws.bufferedAmount > WS_CHUNK_SIZE / 2 && ws.readyState === WebSocket.OPEN) {
          await new Promise(r => setTimeout(r, 5));
        }
        const end = Math.min(p + WS_CHUNK_SIZE, buf.length);
        ws.send(buf.subarray(p, end));
        p = end;
      }
    };

    await sendBuf(toc);
    // Surface periodic progress updates between files even though the server
    // can't tell us exactly which file we're on -- estimate from byte count.
    let bytesSent = toc.length;
    for (let i = 0; i < items.length; i++) {
      if (isCancelled && isCancelled()) throw new Error('Cancelled by user');
      await sendBuf(items[i].bytes);
      bytesSent += items[i].bytes.length;
      if (onProgress) onProgress(i + 1, items.length, bytesSent);
    }

    // Drain any PACK_PROGRESS messages and wait for PACK_DONE.
    const doneDeadline = Date.now() + 60000;
    while (Date.now() < doneDeadline) {
      const remaining = doneDeadline - Date.now();
      const msg = await readMsg(Math.max(1000, remaining));
      if (msg === 'PACK_DONE') {
        return { ok: true, uploaded: items.length, totalBytes };
      }
      if (typeof msg === 'string' && msg.startsWith('PACK_PROGRESS:')) continue;
      if (typeof msg === 'string' && msg.startsWith('PACK_ERROR:')) {
        return { ok: false, error: msg.substring(11), uploaded: 0, totalBytes };
      }
    }
    return { ok: false, error: 'PACK_DONE timeout', uploaded: 0, totalBytes };
  } finally {
    try { ws.close(); } catch (_) {}
  }
}

// CrumBLE 4.5.5+: persistent-WebSocket cache uploader. The per-file HTTP POST
// path below (uploadOneToDevice) opens a fresh TCP+HTTP connection for each
// cache file -- 248 files in a typical CJK book means 248 connection setups,
// 248 multipart parses on the ESP32-C3, and 248 heap-reclaim cycles in the
// FT handler. Each cycle leaves a sliver of fragmentation behind, and after
// ~80-150 files MaxAlloc falls below the 3072-byte WebActivity floor and the
// device silent-restarts mid-batch. Resume picks up but the cycle repeats.
//
// This helper opens ONE WebSocket for the whole batch and reuses it across
// every file. The existing WS protocol (START / data / DONE) already accepts
// back-to-back STARTs on the same connection -- server resets
// wsUploadInProgress=false on DONE so the next START is honored. With per-
// file TCP/HTTP overhead removed, fragmentation accumulates an order of
// magnitude slower; the typical batch should complete without tripping the
// safety net. When the WS does die mid-batch (real network blip or
// genuine device restart), we reopen and continue from the current file --
// the server's RESUME:<offset> response handles partial writes inside the
// failing file.
//
// Inputs:
//   items: [{ relPath, bytes, destDir, fname }, ...]
//   onProgress(uploadedCount, totalCount, totalBytesSent): called after each
//     successful file
//   isCancelled: () => bool, polled between files
// Returns: { uploaded, failed, totalBytes }
async function uploadCacheFilesPersistentWs(items, onProgress, isCancelled) {
  const WS_CHUNK_SIZE = 4096;
  const READY_TIMEOUT_MS = 8000;
  const DONE_TIMEOUT_MS = 30000;
  const MAX_ATTEMPTS_PER_FILE = 4;
  let uploaded = 0;
  let failed = 0;
  let totalBytes = 0;
  let nextIndex = 0;
  let attemptsForCurrent = 0;

  // Tiny FIFO queue for inbound WS messages, plus a Promise-based reader.
  // The standard WS API only allows ONE onmessage handler, so we route every
  // inbound frame through this queue and let the per-file driver await it.
  const makeMsgQueue = () => {
    const queue = [];
    const waiters = [];
    let closed = false;
    let closeReason = null;
    return {
      push(msg) {
        if (waiters.length > 0) {
          const w = waiters.shift();
          clearTimeout(w.timer);
          w.resolve(msg);
        } else {
          queue.push(msg);
        }
      },
      close(reason) {
        closed = true;
        closeReason = reason || 'closed';
        while (waiters.length > 0) {
          const w = waiters.shift();
          clearTimeout(w.timer);
          w.reject(new Error(`WS ${closeReason}`));
        }
      },
      read(timeoutMs) {
        return new Promise((resolve, reject) => {
          if (queue.length > 0) { resolve(queue.shift()); return; }
          if (closed) { reject(new Error(`WS ${closeReason}`)); return; }
          const timer = setTimeout(() => {
            const idx = waiters.findIndex(w => w.resolve === resolve);
            if (idx >= 0) waiters.splice(idx, 1);
            reject(new Error('WS message timeout'));
          }, timeoutMs);
          waiters.push({ resolve, reject, timer });
        });
      },
    };
  };

  const openWs = () => new Promise((resolve, reject) => {
    const ws = new WebSocket(getWsUrl());
    const msgQueue = makeMsgQueue();
    const onOpenTimer = setTimeout(() => {
      try { ws.close(); } catch (_) {}
      reject(new Error('WS open timeout'));
    }, 10000);
    ws.onopen = () => {
      clearTimeout(onOpenTimer);
      ws.onmessage = (evt) => msgQueue.push(evt.data);
      ws.onerror = () => msgQueue.close('error');
      ws.onclose = () => msgQueue.close('closed');
      resolve({ ws, msgQueue });
    };
    ws.onerror = () => {
      clearTimeout(onOpenTimer);
      reject(new Error('WS open failed'));
    };
  });

  // Upload ONE file on an already-open WS. Throws on any error; caller
  // decides whether to retry (same conn or after reopen).
  const uploadOne = async ({ ws, msgQueue }, item) => {
    const { destDir, fname, bytes } = item;
    if (ws.readyState !== WebSocket.OPEN) throw new Error('WS not open');
    ws.send(`START:${fname}:${bytes.length}:${destDir}`);
    const startMsg = await msgQueue.read(READY_TIMEOUT_MS);
    let resumeOffset = 0;
    if (typeof startMsg === 'string' && startMsg.startsWith('RESUME:')) {
      resumeOffset = parseInt(startMsg.substring(7), 10) || 0;
    } else if (typeof startMsg === 'string' && (startMsg === 'DONE' || startMsg.startsWith('DONE:'))) {
      // Zero-byte sentinel/marker files (bake-fp-*, prebake-*.marker)
      // are completed by the device the moment the START is received --
      // no chunks follow, so the device fires DONE immediately. Prior
      // to this fix the browser bailed with "Unexpected START response"
      // and the markers never got created, which made the device think
      // the cache wasn't fully baked on the next boot.
      return;
    } else if (startMsg !== 'READY') {
      if (typeof startMsg === 'string' && startMsg.startsWith('ERROR:')) {
        throw new Error(startMsg.substring(6));
      }
      throw new Error(`Unexpected START response: ${startMsg}`);
    }
    // Stream chunks with bufferedAmount throttle. Same single-chunk-in-
    // flight discipline FilesPage.html uses for the EPUB upload -- keeps
    // the device's recv buffer from outpacing SD write throughput.
    let offset = resumeOffset;
    while (offset < bytes.length) {
      if (ws.readyState !== WebSocket.OPEN) throw new Error('WS closed during send');
      while (ws.bufferedAmount > WS_CHUNK_SIZE / 2 && ws.readyState === WebSocket.OPEN) {
        await new Promise(r => setTimeout(r, 5));
      }
      const end = Math.min(offset + WS_CHUNK_SIZE, bytes.length);
      // Send a fresh Uint8Array slice each time so the underlying buffer
      // isn't held across multiple ws.send calls (some browsers keep a
      // reference to the buffer until the queued frame drains).
      ws.send(bytes.subarray(offset, end));
      offset = end;
    }
    // Wait for DONE, consuming any interim PROGRESS messages.
    const doneDeadline = Date.now() + DONE_TIMEOUT_MS;
    while (Date.now() < doneDeadline) {
      const remaining = doneDeadline - Date.now();
      const msg = await msgQueue.read(Math.max(1000, remaining));
      if (msg === 'DONE' || (typeof msg === 'string' && msg.startsWith('DONE:'))) return;
      if (typeof msg === 'string' && msg.startsWith('PROGRESS:')) continue;
      if (typeof msg === 'string' && msg.startsWith('ERROR:')) {
        throw new Error(msg.substring(6));
      }
      // Any other message: ignore (forward-compat with future protocol additions)
    }
    throw new Error('DONE timeout');
  };

  const sleep = (ms) => new Promise(r => setTimeout(r, ms));

  while (nextIndex < items.length) {
    if (isCancelled && isCancelled()) {
      log(`Cache upload cancelled at ${nextIndex}/${items.length}`, '', 'PRE');
      break;
    }
    let conn = null;
    try {
      conn = await openWs();
    } catch (e) {
      attemptsForCurrent++;
      log(`WS open failed (attempt ${attemptsForCurrent}): ${e.message}`, 'warning', 'PRE-FAIL');
      if (attemptsForCurrent >= MAX_ATTEMPTS_PER_FILE) {
        failed++;
        nextIndex++;
        attemptsForCurrent = 0;
        continue;
      }
      await sleep(2000 * attemptsForCurrent);
      continue;
    }
    try {
      while (nextIndex < items.length && conn.ws.readyState === WebSocket.OPEN) {
        if (isCancelled && isCancelled()) break;
        const item = items[nextIndex];
        try {
          await uploadOne(conn, item);
          uploaded++;
          totalBytes += item.bytes.length;
          nextIndex++;
          attemptsForCurrent = 0;
          if (onProgress) onProgress(uploaded, items.length, totalBytes);
        } catch (e) {
          attemptsForCurrent++;
          log(`Upload error ${item.relPath} (attempt ${attemptsForCurrent}/${MAX_ATTEMPTS_PER_FILE}): ${e.message}`,
              'warning', 'PRE-FAIL');
          if (attemptsForCurrent >= MAX_ATTEMPTS_PER_FILE) {
            failed++;
            nextIndex++;
            attemptsForCurrent = 0;
            // Don't tear down the WS just because one file fails terminally;
            // the next file may still go through on this connection.
            continue;
          }
          // For a non-terminal error, break out so we reopen the WS. Most
          // mid-file failures here are connection-level (closed/timeout),
          // which would leave the server's wsUploadInProgress flag in an
          // ambiguous state -- a clean reconnect is the safest path.
          break;
        }
      }
    } finally {
      try { conn.ws.close(); } catch (_) {}
    }
    // Brief backoff if we exited the inner loop with files remaining AND the
    // exit wasn't a clean cancel. Gives the device a moment if it just
    // restarted under heap pressure.
    if (nextIndex < items.length && !(isCancelled && isCancelled())) {
      await sleep(800);
    }
  }
  return { uploaded, failed, totalBytes };
}

async function uploadOneToDevice(destDir, name, bytes) {
  const formData = new FormData();
  // The handler infers the filename from the upload's part name; we set it
  // explicitly so the destination basename matches what we generated.
  formData.append('file', new Blob([bytes]), name);
  // CrumBLE 4.5.4: hard 12s timeout. The default browser fetch timeout
  // is 30-60s (varies), so when the device drops mid-prebake we'd hang
  // the upload loop for half a minute per failed file. With 700+ cache
  // files in a chapter-heavy book that meant a single connection drop
  // could pin the modal at 'uploading cache file 26/703' for 6+ hours
  // before the script gave up. 12s is enough headroom for a healthy
  // device's slowest write while still failing fast on a dead one.
  //
  // CrumBLE 4.5.4: also wire the GLOBAL window.optimizeAbortController so
  // the upload modal's Cancel button can fail-fast every in-flight upload
  // immediately instead of waiting for the 12s timer. Per-upload deadline
  // still applies on top via the local controller.
  const controller = new AbortController();
  const deadline = setTimeout(() => controller.abort(), 12000);
  // Bridge: if the global optimize cancel fires, abort the local controller.
  const globalAbort = window.optimizeAbortController;
  const onGlobalAbort = () => controller.abort();
  if (globalAbort) globalAbort.signal.addEventListener('abort', onGlobalAbort);
  try {
    const resp = await fetch(`/upload?path=${encodeURIComponent(destDir)}`, {
      method: 'POST',
      body: formData,
      signal: controller.signal,
    });
    return resp;
  } finally {
    clearTimeout(deadline);
    if (globalAbort) globalAbort.signal.removeEventListener('abort', onGlobalAbort);
  }
}

// POST /mkdir?path=<parent>&name=<dir>. Returns 200 on create, 400 if
// already exists (which is fine -- the caller treats both as success).
async function ensureRemoteDir(parentPath, dirName) {
  const url = `/mkdir?path=${encodeURIComponent(parentPath)}&name=${encodeURIComponent(dirName)}`;
  try {
    await fetch(url, { method: 'POST' });
  } catch (e) {
    // Network errors are surfaced by the subsequent upload; mkdir failures
    // (including "already exists") are not fatal in themselves.
  }
}

// Run the WASM prebake against `epubBlob` and stream the resulting cache
// files to the device. `deviceFilePath` is the FULL absolute SD-card path
// of the EPUB (e.g. "/Books/MyBook.epub"), NOT just the basename --
// Epub::cachePathForFilePath on the device hashes the whole path string,
// so if we only pass "/MyBook.epub" while the device sees "/Books/
// MyBook.epub", the hashes differ and the prebake files land in a cache
// dir the device doesn't look at. Caller is responsible for joining the
// upload's currentPath with the filename.
//
// Calls progressCallback(donePct) with values 0..100 across the whole run
// (download settings -> run WASM -> upload N files).
//
// Throws on any fatal error. Returns a summary object on success:
//   { hashId, uploaded, failed, totalBytes, elapsedMs }
async function prebakeChapters(epubBlob, deviceFilePath, progressCallback, options) {
  // CrumBLE 4.5.4: opts.collectOnly skips the per-file device upload at
  // the end and returns the in-memory cache file list to the caller.
  // Used by the local-folder output path: caller writes the same files
  // into a user-picked directory via the File System Access API (or zip
  // fallback). hashId still comes back so the caller can mirror the
  // /.crosspoint/<hashId>/ layout on disk.
  const collectOnly = !!(options && options.collectOnly);
  // CrumBLE 4.5.5: opts.overrideRenderInfo lets the caller supply a
  // pre-built renderInfo object (typically the dryRun response from
  // /api/save-reader-settings via the preflight modal). When present we
  // skip the /api/reader-render-info fetch below -- the modal already
  // captured the user's bake-time choices and the device's persistent
  // SETTINGS may not reflect them (that's the whole point: pick a font
  // for THIS bake without rewriting the device). Without this hook the
  // bake always pulled the device's persisted settings and the modal
  // choices were ignored.
  const overrideRenderInfo = (options && options.overrideRenderInfo) || null;
  const startTime = Date.now();
  // progressCallback signature: (percent, optionalStatusText). The caller
  // updates the progress bar with `percent` and the caption with the
  // status text when present. Wraps in try/catch because the caller can
  // also throw inside the callback (e.g. on cancellation paths) and we
  // don't want a UI error to abort the prebake.
  const reportProgress = (pct, status) => {
    if (progressCallback) {
      try { progressCallback(Math.max(0, Math.min(100, pct)), status); } catch (e) {}
    }
  };
  reportProgress(0, 'starting prebake...');

  // 1. Fetch live render-info from the device (same endpoint the BT optimizer
  //    uses for .pxc baking). Required because the CLI hard-errors on
  //    fitVersion < 2 -- the eight layout fields it adds are baked into
  //    section file headers and must match the device's current state.
  //    CrumBLE 4.5.5: skip the fetch when the caller supplied an
  //    overrideRenderInfo (the preflight modal's dryRun response). The
  //    override already carries all required fields plus the user's
  //    bake-time selections that don't (and shouldn't) live in device
  //    SETTINGS yet.
  let renderInfo;
  if (overrideRenderInfo) {
    log(`Chapter prebake: using preflight-modal renderInfo override (sdFontFamilyName='${overrideRenderInfo.sdFontFamilyName || ''}' fontSize=${overrideRenderInfo.fontSize | 0})`, '', 'PRE');
    renderInfo = overrideRenderInfo;
  } else {
    log('Chapter prebake: fetching device render settings...', '', 'PRE');
    const riResp = await fetch('/api/reader-render-info');
    if (!riResp.ok) {
      throw new Error(`render-info fetch failed: HTTP ${riResp.status}`);
    }
    renderInfo = await riResp.json();
  }
  if (!(renderInfo.fitVersion >= 2)) {
    throw new Error(`device reports fitVersion=${renderInfo.fitVersion}; chapter prebake needs >=2 (update firmware)`);
  }
  reportProgress(5, 'loading WASM module...');

  // 2. Boot the WASM worker (downloads ~850 KB gz on first call). All
  //    Module/FS state lives inside the worker now -- main thread no
  //    longer touches Module directly, so the worker can run callMain
  //    without blocking the UI thread (the entire reason for the move).
  log('Chapter prebake: spinning up WASM worker...', '', 'PRE');
  await ensurePrebakeWorker();
  reportProgress(15, 'building chapter index...');

  // 3. Read the EPUB into a transferable ArrayBuffer. The worker will
  //    stage it (and the settings JSON, and the optional SD font) into
  //    its own MEMFS when it receives the 'run' message. We capture
  //    these on the main thread because some inputs come from device
  //    HTTP fetches that we want to do without holding worker time.
  log('Chapter prebake: preparing input for worker...', '', 'PRE');
  const epubBytes = new Uint8Array(await epubBlob.arrayBuffer());
  // Paths are inside the worker MEMFS -- the worker creates them.
  const inputPath = '/input.epub';
  const settingsPath = '/settings.json';
  const outDir = '/out';
  const sdFontPath = '/sd_font.cpfont';
  let sdFontBytesForWorker = null;

  // 4. Run prebake. callMain returns the CLI's exit code. The CLI's
  //    --device-path tells it which SD-card path to hash for the cache
  //    directory name -- without this it'd hash "/input.epub" instead
  //    of the real device-side path.
  // deviceFilePath comes in already absolute from the caller; verify and
  // fall back defensively so we don't quietly hash a relative path.
  let devicePath = deviceFilePath || '';
  if (!devicePath.startsWith('/')) devicePath = '/' + devicePath;
  // CrumBLE 4.2: --skip-thumbs flag dropped. WASM-side prebakeAllThumbs
  // can decode the cover JPEG/PNG, dither it for e-ink, and ship the
  // resulting BMPs alongside the section files. The device's first
  // home-render no longer has to do the heap-pressured decode itself,
  // which is the difference between a snappy carousel and a "loading"
  // placeholder on first cover view post-upload. Adds ~5-30 KB per
  // thumb size × 3 sizes to the upload payload; covers that fail to
  // decode (e.g. progressive JPEGs the vendored JPEGDEC chokes on)
  // fall through to the device-side generator -- same fallback that
  // existed before, just hit less often.
  const cliArgs = [
    '--settings-file', settingsPath,
    '--output-dir', outDir,
    '--device-path', devicePath,
    inputPath,
  ];

  // CrumBLE 4.5.5+: lock the layout fields the preflight modal collected
  // into the bake as authoritative CLI overrides. The settings file should
  // already reflect these (dryRun returns them in renderInfo), but the
  // overrides act as a belt-and-suspenders guard against any path that
  // produces a stale settings file -- e.g. when the modal is dismissed
  // without the user editing a field, the dryRun trip doesn't always
  // converge to the device's current value, and the fingerprint mismatch
  // prompt fires on every open. Passing the renderInfo value here as the
  // override locks it independent of how the settings file was built.
  // Mirrors how --sd-font-family/--sd-font-size already get unshifted.
  if (typeof renderInfo.paragraphAlignment === 'number') {
    cliArgs.unshift('--paragraph-alignment', String(renderInfo.paragraphAlignment | 0));
  }
  if (typeof renderInfo.extraParagraphSpacing === 'number') {
    cliArgs.unshift('--extra-paragraph-spacing', String(renderInfo.extraParagraphSpacing | 0));
  }
  if (typeof renderInfo.lineCompression === 'number' && renderInfo.lineCompression > 0) {
    // Keep 3 decimal places -- the device's manifest format and the
    // fingerprint comparison both round to this precision.
    cliArgs.unshift('--line-compression', renderInfo.lineCompression.toFixed(3));
  }

  // CrumBLE 4.2: when the user picked an SD-card font in the preflight
  // modal, fetch that font's .cpfont bytes from the device and pass them
  // into WASM via MEMFS. The CLI loads the font, computes the same fontId
  // the device-side reader will resolve (FNV hash over contentHash +
  // family + pt-size), and registers it in the renderer so layout calls
  // for that fontId produce real glyph metrics instead of zero-height
  // lines. Without this path, an SD-font prebake bakes layouts against
  // whichever default font the WASM happened to have, and the device's
  // section-load fingerprint check fails on every open.
  log(`SD font fetch check: sdFontFamilyName='${renderInfo.sdFontFamilyName || ''}' sdFontPickedPointSize=${renderInfo.sdFontPickedPointSize | 0}`, '', 'PRE');
  if (renderInfo.sdFontFamilyName && renderInfo.sdFontFamilyName.length > 0) {
    const fname = renderInfo.sdFontFamilyName;
    // CrumBLE 4.2: fall back to fetching the font list and guessing the
    // best size if sdFontPickedPointSize wasn't surfaced (e.g. modal
    // opened on an SD font already selected and the per-modal reactive
    // dropdown didn't fire because the user didn't change anything).
    let pt = renderInfo.sdFontPickedPointSize | 0;
    if (!pt) {
      try {
        const fontsData = await fetch('/api/fonts').then(r => r.json());
        const family = (fontsData.families || []).find(f => f.name === fname);
        if (family && family.sizes && family.sizes.length > 0) {
          const sorted = family.sizes.slice().sort((a, b) => (a | 0) - (b | 0));
          // Try to pick the size closest to the device's stored fontSize
          // interpreted as an index into the SD family. If fontSize is
          // larger than the array, clamp to the last entry.
          const idx = Math.min(sorted.length - 1, Math.max(0, renderInfo.fontSize | 0));
          pt = sorted[idx];
          log(`SD font fallback: guessed pt=${pt} from family.sizes[${idx}] (fontSize=${renderInfo.fontSize})`, '', 'PRE');
        }
      } catch (e) {
        log(`SD font fallback fetch failed: ${e.message || e}`, '', 'WARN');
      }
    }
    if (pt > 0) {
      log(`Chapter prebake: fetching SD font ${fname} @ ${pt}pt`, '', 'PRE');
      // CrumBLE 4.5.5+: cheap version probe + IndexedDB cache short-circuit
      // BEFORE the big WiFi pull. The probe returns {fileSize, mtime} for
      // this family+pt -- ~100 bytes, ~50ms. If the browser already cached
      // the bytes that match (family|pt|mtime|fileSize), we skip the 30-40s
      // streaming transfer entirely. Cache invalidates automatically when
      // the user re-uploads the font (FAT mtime bumps).
      let cachedFontBytes = null;
      let fontVer = null;
      let fontFingerprint = null;
      const cacheProbeStart = performance.now();
      try {
        const verResp = await fetch(
          `/api/fonts/version?family=${encodeURIComponent(fname)}&size=${pt}`,
          { cache: 'no-store' });
        if (verResp.ok) {
          fontVer = await verResp.json();
        } else {
          log(`Font version probe: HTTP ${verResp.status} -- proceeding without cache`, '', 'PRE');
        }
      } catch (e) {
        log(`Font version probe failed (${e.message || e}) -- proceeding without cache`, '', 'PRE');
      }
      if (fontVer && typeof fontVer.mtime === 'number' && typeof fontVer.fileSize === 'number') {
        fontFingerprint = `${fname}|${pt}|${fontVer.mtime}|${fontVer.fileSize}`;
        const cacheHit = await fontCacheGet(fontFingerprint);
        const probeMs = (performance.now() - cacheProbeStart).toFixed(0);
        if (cacheHit) {
          cachedFontBytes = new Uint8Array(cacheHit);
          log(`Font cache HIT in ${probeMs}ms: ${fname} @ ${pt}pt (${(cachedFontBytes.length / 1024).toFixed(0)} KB cached, mtime=${fontVer.mtime})`,
              '', 'PRE-OK');
        } else {
          log(`Font cache MISS in ${probeMs}ms (mtime=${fontVer.mtime}, fileSize=${fontVer.fileSize}) -- fetching from device...`, '', 'PRE');
        }
      }
      // Hard-deadline + retry. Without this, a hung HTTP response from the
      // device (most often heap pressure stalling /api/fonts/file mid-stream)
      // silently wedges the whole prebake at the "fetching SD font" log line
      // with no progress and no error -- user can't tell it's stuck vs slow.
      // CrumBLE 4.5.4: bumped 30s -> 120s. Full-coverage CJK families like
      // LXGWWenKai are 3-5 MB at 14pt; over the device's WiFi+SD pipeline
      // (1 KB chunks at the time, ~50 KB/s SD reads on a busy heap) the
      // 30s ceiling didn't clear with margin.
      // CrumBLE 4.5.5+: pivoted to STREAMING reader + idle-timeout instead
      // of a single hard request deadline. The old approach waited 120s
      // before aborting even when the connection was making no progress;
      // by then the device's WEB task was wedged trying to write to the
      // aborted socket (it lacked a connected() check in the streaming
      // loop) and subsequent retries hit "Failed to fetch" because no
      // TCP accept slot was free. The streaming reader lets us:
      //   - measure per-chunk progress (KB/s, bytes received)
      //   - abort fast when bytes stop arriving (idle timeout < total
      //     timeout), so the device-side abort detection kicks in and
      //     frees up the WEB task before the retry
      //   - emit per-second progress logs the user can read, instead
      //     of the silent 120s wait we had before
      const FONT_FETCH_TOTAL_TIMEOUT_MS = 90000;   // overall ceiling
      const FONT_FETCH_IDLE_TIMEOUT_MS = 15000;    // no bytes for 15s -> abort
      const FONT_FETCH_MAX_ATTEMPTS = 3;
      let fontResp = null;
      let fontBytes = cachedFontBytes;  // pre-seeded by IDB cache short-circuit above
      let lastFontErr = null;
      for (let attempt = 1; fontBytes === null && attempt <= FONT_FETCH_MAX_ATTEMPTS; attempt++) {
        const ctrl = new AbortController();
        const fetchStart = performance.now();
        let lastByteAt = fetchStart;
        const totalDeadline = setTimeout(() => {
          log(`Font fetch: total timeout (${FONT_FETCH_TOTAL_TIMEOUT_MS}ms) reached, aborting`, 'warning', 'PRE');
          ctrl.abort();
        }, FONT_FETCH_TOTAL_TIMEOUT_MS);
        let idleCheckInterval = null;
        try {
          log(`Font fetch attempt ${attempt}: dispatching GET /api/fonts/file`, '', 'PRE');
          fontResp = await fetch(
            `/api/fonts/file?family=${encodeURIComponent(fname)}&size=${pt}`,
            { signal: ctrl.signal },
          );
          const headersAtMs = performance.now() - fetchStart;
          log(`Font fetch: HTTP ${fontResp.status} response in ${headersAtMs.toFixed(0)}ms`, '', 'PRE');
          if (!fontResp.ok) throw new Error(`HTTP ${fontResp.status}`);
          const contentLength = parseInt(fontResp.headers.get('content-length') || '0', 10);
          // Stream the body so we can detect mid-transfer hangs via an
          // idle timeout (no bytes for N seconds) AND emit progress logs.
          if (!fontResp.body || typeof fontResp.body.getReader !== 'function') {
            // Older browsers without streaming -- fall back to arrayBuffer.
            log(`Font fetch: ReadableStream not available, falling back to arrayBuffer`, '', 'PRE');
            fontBytes = new Uint8Array(await fontResp.arrayBuffer());
          } else {
            idleCheckInterval = setInterval(() => {
              const sinceLastByte = performance.now() - lastByteAt;
              if (sinceLastByte > FONT_FETCH_IDLE_TIMEOUT_MS) {
                log(`Font fetch: idle for ${(sinceLastByte / 1000).toFixed(1)}s (no bytes received), aborting`, 'warning', 'PRE');
                ctrl.abort();
              }
            }, 1000);
            const reader = fontResp.body.getReader();
            const chunks = [];
            let receivedBytes = 0;
            let lastProgressLog = fetchStart;
            while (true) {
              const { done, value } = await reader.read();
              if (done) break;
              chunks.push(value);
              receivedBytes += value.length;
              lastByteAt = performance.now();
              if (lastByteAt - lastProgressLog > 1500) {
                const elapsed = lastByteAt - fetchStart;
                const kbs = elapsed > 0 ? (receivedBytes / elapsed) : 0;
                const pct = contentLength > 0 ? ((receivedBytes / contentLength) * 100).toFixed(0) : '?';
                log(`Font fetch: ${receivedBytes} / ${contentLength || '?'} bytes (${pct}%) @ ${kbs.toFixed(0)} KB/s`, '', 'PRE');
                lastProgressLog = lastByteAt;
              }
            }
            fontBytes = new Uint8Array(receivedBytes);
            let offset = 0;
            for (const c of chunks) { fontBytes.set(c, offset); offset += c.length; }
          }
          clearTimeout(totalDeadline);
          if (idleCheckInterval) clearInterval(idleCheckInterval);
          const totalMs = performance.now() - fetchStart;
          const avgKbs = totalMs > 0 ? (fontBytes.length / totalMs) : 0;
          log(`Font fetch: ${fontBytes.length} bytes in ${totalMs.toFixed(0)}ms (avg ${avgKbs.toFixed(0)} KB/s)`, '', 'PRE');
          lastFontErr = null;
          break;
        } catch (e) {
          clearTimeout(totalDeadline);
          if (idleCheckInterval) clearInterval(idleCheckInterval);
          lastFontErr = e;
          const elapsed = (performance.now() - fetchStart).toFixed(0);
          log(`SD font fetch attempt ${attempt}/${FONT_FETCH_MAX_ATTEMPTS} failed after ${elapsed}ms: ${e.name === 'AbortError' ? 'aborted (timeout)' : (e.message || e)}`,
              'warning', 'PRE');
          if (attempt < FONT_FETCH_MAX_ATTEMPTS) {
            const backoff = 1500 * attempt;
            log(`Retrying in ${backoff}ms...`, '', 'PRE');
            await new Promise(r => setTimeout(r, backoff));
          }
        }
      }
      try {
        if (!fontBytes) {
          throw lastFontErr || new Error('SD font fetch exhausted retries');
        }
        // CrumBLE 4.5.5+: write the freshly-fetched bytes into the IDB cache
        // so subsequent prebakes of any other book using the same font+size
        // skip the WiFi pull entirely. Skipped on a cache hit (already in
        // IDB) -- gated on fontFingerprint + cachedFontBytes==null. Eviction
        // pass after each write keeps the store under 50 MB.
        if (fontFingerprint && !cachedFontBytes) {
          fontCachePut(fontFingerprint, fname, pt, fontVer.mtime, fontBytes)
            .then(() => fontCacheEvictLRU(FONT_CACHE_BUDGET_BYTES))
            .then(() => log(`Font cached for next time: ${fontFingerprint}`, '', 'PRE'))
            .catch(() => {});  // best-effort
        }
        // Capture for the worker -- it'll Module.FS.writeFile inside its
        // own MEMFS when the run message arrives. Same path string so
        // the CLI args we built below still point at the right file.
        sdFontBytesForWorker = fontBytes;
        // CrumBLE 4.4: --emit-section-glyph-subsets gates BOTH the v39 EGS
        // emit AND the v40 glyph atlas emit on the CLI side. Without it the
        // section files come out v40-format but with the atlas trailer at
        // 0/0/0 -- the device then has nothing to install at section open
        // and falls back to live SD-font glyph reads (which crater the
        // heap on chapter boundaries -> question marks in the reader).
        // This was the whole reason for shipping atlas in the first place,
        // so when the user pays the SD-font prebake cost they should get
        // the atlas payload too.
        cliArgs.unshift('--sd-font-path', sdFontPath,
                        '--sd-font-family', fname,
                        '--sd-font-size', String(pt),
                        '--emit-section-glyph-subsets');
        log(`SD font passed to WASM: path=${sdFontPath} family=${fname} pt=${pt} + atlas/EGS emit enabled`, '', 'PRE');
      } catch (e) {
        log(`SD font fetch failed (${e.message || e}) -- bake will use default font; prebake will not load cleanly`,
            '', 'WARN');
      }
    } else {
      log(`SD font: no point size resolved -- bake will use default font`, '', 'WARN');
    }
  }
  log(`Chapter prebake: dispatching to worker callMain(${JSON.stringify(cliArgs)})`, '', 'PRE');
  // Clear the print/printErr capture buffer right before we run so any
  // earlier session output doesn't pollute this run's diagnostics.
  crumblePrebakeOutputBuf.length = 0;
  // CrumBLE 4.5.4: heartbeat now runs on the MAIN thread while WASM
  // executes inside the worker. The progress bar actually animates --
  // CSS shimmer/ellipsis + the JS-driven % both update smoothly because
  // the main thread isn't blocked. Each stdout line from WASM lands as
  // a postMessage event, fires the heartbeat, and the browser paints
  // before WASM's next stdout. No more frozen percentage during long
  // section layouts.
  let heartbeatPct = 40;
  crumblePrebakeHeartbeat = (line) => {
    heartbeatPct = Math.min(89, heartbeatPct + 0.4);
    const summary = typeof line === 'string' && line.length > 70 ? line.slice(0, 67) + '...' : (line || '');
    reportProgress(heartbeatPct, summary || 'baking sections...');
  };
  reportProgress(heartbeatPct, 'Building chapter layout (can take several minutes for large books)...');

  // Run the WASM prebake in the worker. We transfer the EPUB buffer (and
  // SD font, if any) so the main thread doesn't keep a duplicate copy.
  // The worker walks /out/.crosspoint after callMain succeeds and posts
  // back {hashId, files: [{relPath, buffer, size}]} with each buffer as
  // a transferable -- zero-copy back across the boundary.
  const worker = await ensurePrebakeWorker();
  // CRITICAL: detach the input buffers from main-thread reach BEFORE
  // postMessage so the structured-clone path picks transferable mode.
  // We slice() to get a fresh ArrayBuffer (avoid messing with the
  // arrayBuffer() result the caller might still hold).
  const epubBufferForTransfer = epubBytes.buffer.slice(epubBytes.byteOffset,
                                                       epubBytes.byteOffset + epubBytes.byteLength);
  const sdFontBufferForTransfer = sdFontBytesForWorker
      ? sdFontBytesForWorker.buffer.slice(sdFontBytesForWorker.byteOffset,
                                          sdFontBytesForWorker.byteOffset + sdFontBytesForWorker.byteLength)
      : null;
  const transferList = [epubBufferForTransfer];
  if (sdFontBufferForTransfer) transferList.push(sdFontBufferForTransfer);

  const workerResult = await new Promise((resolve, reject) => {
    const onMsg = (e) => {
      const msg = e.data;
      if (!msg) return;
      if (msg.type === 'stdout') {
        crumblePrebakeOutputBuf.push(msg.line);
        try { console.log('[prebake]', msg.line); } catch (e) {}
        try { log(`[prebake] ${msg.line}`, '', 'PRE'); } catch (e) {}
        if (typeof crumblePrebakeHeartbeat === 'function') {
          try { crumblePrebakeHeartbeat(msg.line); } catch (e) {}
        }
      } else if (msg.type === 'stderr') {
        crumblePrebakeOutputBuf.push(msg.line);
        try { console.error('[prebake-err]', msg.line); } catch (e) {}
        try { log(`[prebake-err] ${msg.line}`, 'warning', 'PRE-ERR'); } catch (e) {}
        if (typeof crumblePrebakeHeartbeat === 'function') {
          try { crumblePrebakeHeartbeat(msg.line); } catch (e) {}
        }
      } else if (msg.type === 'done') {
        worker.removeEventListener('message', onMsg);
        worker.removeEventListener('error', onErr);
        resolve({ hashId: msg.hashId, files: msg.files });
      } else if (msg.type === 'error') {
        worker.removeEventListener('message', onMsg);
        worker.removeEventListener('error', onErr);
        const captured = crumblePrebakeOutputBuf.slice(-12).join('\n');
        const detail = captured ? `\n--- CLI output ---\n${captured}` : '';
        reject(new Error((msg.message || 'prebake worker error') + detail));
      }
    };
    const onErr = (err) => {
      worker.removeEventListener('message', onMsg);
      worker.removeEventListener('error', onErr);
      reject(new Error('prebake worker error: ' + (err.message || err.type || 'unknown')));
    };
    worker.addEventListener('message', onMsg);
    worker.addEventListener('error', onErr);
    worker.postMessage({
      cmd: 'run',
      epubBytes: epubBufferForTransfer,
      settingsJson: JSON.stringify(renderInfo),
      sdFontBytes: sdFontBufferForTransfer,
      cliArgs,
    }, transferList);
  }).finally(() => {
    crumblePrebakeHeartbeat = null;
  });

  reportProgress(50, 'uploading cache files...');

  const hashId = workerResult.hashId;
  const deviceCacheDir = `/.crosspoint/${hashId}`;

  // CrumBLE 4.5.4: post-prebake atlas-marker check. Field report: CJK books
  // produced only IMG+CHAP badges (no CP.FONT) even when the SD font was
  // selected. The check now consults the file list returned by the worker
  // instead of poking the worker's MEMFS directly.
  try {
    const topLevelEntries = workerResult.files
      .filter((f) => !f.relPath.includes('/'))
      .map((f) => f.relPath);
    const hasCpfontMarker = topLevelEntries.includes('prebake-cpfont.marker');
    const hasSectionsDir = workerResult.files.some((f) => f.relPath.startsWith('sections-prebake/'));
    const hasChapMarker = topLevelEntries.includes('prebake-chap.marker') || hasSectionsDir;
    const sdFontArgsPassed = cliArgs.includes('--sd-font-path');
    log(`Prebake output check: chap=${hasChapMarker ? 'yes' : 'no'} cpfont=${hasCpfontMarker ? 'yes' : 'no'} sdFontPassed=${sdFontArgsPassed ? 'yes' : 'no'}`,
        '', 'PRE');
    if (sdFontArgsPassed && !hasCpfontMarker) {
      log(`SD font was passed to WASM but no atlas marker produced -- CP.FONT badge will be missing. ` +
          `Likely the font's .cpfont doesn't cover the chapter's codepoints, or atlas emit failed mid-bake. ` +
          `Check CLI stdout above for [PRE]/[ATLAS]/[GLYPH] lines.`,
          'warning', 'PRE');
    } else if (!sdFontArgsPassed && renderInfo.sdFontFamilyName && renderInfo.sdFontFamilyName.length > 0) {
      log(`SD font '${renderInfo.sdFontFamilyName}' is selected on device but optimizer didn't push it to WASM. ` +
          `Earlier 'SD font fetch attempt' lines explain why.`,
          'warning', 'PRE');
    }
  } catch (e) {
    log(`atlas-marker check failed: ${e.message || e}`, '', 'PRE');
  }

  // Files come pre-collected from the worker with relPath + transferred
  // ArrayBuffers. Wrap each ArrayBuffer in a Uint8Array view for the
  // upload (or local-collect) step.
  let allFiles = workerResult.files.map((f) => ({
    relPath: f.relPath,
    bytes: new Uint8Array(f.buffer),
  }));

  // CrumBLE 4.5.4 collect-only short-circuit: caller wants the files
  // returned for a local-folder write, not pushed to the device. Skip
  // ensureRemoteDir + the upload loop + final cleanup entirely.
  if (collectOnly) {
    const elapsedMs = Date.now() - startTime;
    let totalBytes = 0;
    for (const f of allFiles) totalBytes += f.bytes.length;
    reportProgress(100);
    log(`Chapter prebake (collect-only) done: ${allFiles.length} files (${formatBytes(totalBytes)}) in ${(elapsedMs / 1000).toFixed(1)}s`,
        '', 'PRE-OK');
    return { hashId, files: allFiles, uploaded: 0, failed: 0, totalBytes, elapsedMs, collectOnly: true };
  }

  // Ensure device-side dirs exist.
  await ensureRemoteDir('/.crosspoint', hashId);
  await ensureRemoteDir(deviceCacheDir, 'sections-prebake');

  // CrumBLE 4.5.4 resume-prebake: BEFORE the upload loop, query the
  // device for what's already at the target cache dir. WASM output is
  // deterministic from (epub bytes + render-info + device path) -- the
  // file SET and per-file SIZES are identical across runs as long as
  // those inputs don't change. So a file that exists on device with
  // matching size is guaranteed to be the right bytes. Filter the
  // upload list to only the missing-or-stale entries.
  //
  // Cost: 2 HTTP roundtrips against /api/files (one per directory we
  // care about: the cache root + sections-prebake/). On a healthy
  // device that's ~1 s total; on a churning one it's a few seconds.
  // Worst-case: the listing fails entirely, in which case we fall
  // through to the full upload (same as before, no regression).
  //
  // What this enables: re-running Optimize on a book where a previous
  // run uploaded 383/481 files now uploads ~98 files instead of 481.
  // Recovery from partial failure goes from 30-60 min back to a few
  // minutes.
  // CrumBLE 4.5.5+: compute this bake's fingerprint up front so the resume
  // check can verify the existing cache (if any) was produced by the same
  // bake setup. See computeBakeFingerprint for what goes into it.
  const bakeFingerprint = await computeBakeFingerprint(renderInfo, epubBytes);
  const bakeFingerprintMarker = `bake-fp-${bakeFingerprint}`;
  log(`Bake fingerprint: ${bakeFingerprint}`, '', 'PRE');

  const resumeCheckStart = Date.now();
  let existingByRelPath = new Map();
  try {
    // The endpoint returns one row per direct child (not recursive),
    // so query each subdirectory we know about. Top-level files
    // (book.bin, manifest.json, markers) come from the cache root
    // listing; sections come from sections-prebake/.
    async function listDir(absPath) {
      const ctrl = new AbortController();
      const tmo = setTimeout(() => ctrl.abort(), 8000);
      try {
        const r = await fetch('/api/files?path=' + encodeURIComponent(absPath),
                              { cache: 'no-store', signal: ctrl.signal });
        clearTimeout(tmo);
        if (!r.ok) return [];
        const rows = await r.json();
        return Array.isArray(rows) ? rows : [];
      } catch (e) {
        clearTimeout(tmo);
        return [];
      }
    }
    const topRows = await listDir(deviceCacheDir);
    for (const row of topRows) {
      if (row && row.name && !row.isDirectory) {
        existingByRelPath.set(row.name, row.size | 0);
      }
    }
    const sectionsRows = await listDir(deviceCacheDir + '/sections-prebake');
    for (const row of sectionsRows) {
      if (row && row.name && !row.isDirectory) {
        existingByRelPath.set('sections-prebake/' + row.name, row.size | 0);
      }
    }
    // CrumBLE 4.5.5+: bake-fingerprint guard. If the cache dir already
    // contains files but no marker matching THIS bake's fingerprint, the
    // existing files came from a different bake setup (different WASM,
    // different settings, different EPUB content). Throw them out of the
    // resume map so every file gets re-uploaded. This catches the class of
    // bug where two bake runs produce byte-identically-sized but
    // content-different files and the size-only resume check falsely
    // matches them.
    const hasFreshMarker = topRows.some((row) => row && row.name === bakeFingerprintMarker);
    if (existingByRelPath.size > 0 && !hasFreshMarker) {
      log(`Bake-fingerprint mismatch (no ${bakeFingerprintMarker} found in cache dir) -- ` +
          `treating all ${existingByRelPath.size} existing files as stale, forcing full upload`,
          'warning', 'PRE');
      existingByRelPath = new Map();
    }
  } catch (e) {
    log(`Resume check failed (${e.message || e}) -- falling through to full upload`,
        '', 'PRE');
    existingByRelPath = new Map();
  }
  const resumeCheckMs = Date.now() - resumeCheckStart;
  // CrumBLE 4.5.5+: ship the bake-fingerprint marker as a 0-byte file at the
  // top of the cache dir. Next bake's resume check looks for this filename
  // in the listing; missing/different -> stale cache. Marker filename
  // encodes the fingerprint, so we don't need to read the file's content to
  // verify -- the listing alone is enough. Always uploaded fresh (it's 0
  // bytes; cost is one extra pack-mode TOC entry).
  allFiles.push({ relPath: bakeFingerprintMarker, bytes: new Uint8Array(0) });
  if (existingByRelPath.size > 0) {
    const before = allFiles.length;
    // Keep only files that are MISSING from device OR have a size
    // mismatch (indicates a partial / corrupted prior write -- safer
    // to re-upload than trust). Size match is sufficient because WASM
    // output is byte-deterministic from this run's inputs.
    let alreadyOk = 0;
    let stale = 0;
    allFiles = allFiles.filter((f) => {
      const ex = existingByRelPath.get(f.relPath);
      if (ex === undefined) return true;             // missing
      if (ex !== f.bytes.length) { stale++; return true; }  // size mismatch
      alreadyOk++;
      return false;                                  // already-good, skip
    });
    log(`Resume check (${resumeCheckMs} ms): ${alreadyOk} files already on device, ` +
        `${stale} stale (size mismatch -- re-uploading), ${allFiles.length} to upload (was ${before})`,
        '', 'PRE');
    if (allFiles.length === 0) {
      log(`Chapter prebake: ALL ${before} files already present -- nothing to upload`,
          '', 'PRE-OK');
      reportProgress(100);
      const elapsedMs = Date.now() - startTime;
      return { hashId, uploaded: 0, failed: 0, totalBytes: 0, elapsedMs,
               resumed: true, alreadyPresent: before };
    }
  } else {
    log(`Resume check: no prior files at ${deviceCacheDir} (or listing failed) -- uploading all ${allFiles.length}`,
        '', 'PRE');
  }
  // Numeric-ish sort: top-level non-section files first, then sections by index.
  allFiles.sort((a, b) => {
    const aIsSection = a.relPath.startsWith('sections-prebake/');
    const bIsSection = b.relPath.startsWith('sections-prebake/');
    if (aIsSection !== bIsSection) return aIsSection ? 1 : -1;
    if (aIsSection) {
      const an = parseInt(a.relPath.match(/(\d+)\.bin$/)?.[1] ?? '0', 10);
      const bn = parseInt(b.relPath.match(/(\d+)\.bin$/)?.[1] ?? '0', 10);
      return an - bn;
    }
    return a.relPath.localeCompare(b.relPath);
  });

  // CrumBLE 4.5.5+: pack-mode batch upload, persistent-WS fallback. Pack mode
  // bundles everything into one binary stream (TOC + concatenated payloads)
  // that the device demuxes byte-by-byte -- removes both the per-file
  // protocol framing AND the SD open/close + FATFS sector-cache churn that
  // was the dominant fragmentation source. Falls back to persistent-WS per-
  // file if pack mode is rejected for any reason (older firmware that
  // doesn't recognize PACK_START, heap too low for the pre-flight, etc.) so
  // an older device still works.
  //
  // Items carry both the source bytes and destination so the helper can
  // assemble per-file headers without re-deriving paths.
  const items = allFiles.map(({ relPath, bytes }) => {
    const slash = relPath.lastIndexOf('/');
    const destSubdir = slash >= 0 ? relPath.substring(0, slash) : '';
    const destDir = destSubdir ? `${deviceCacheDir}/${destSubdir}` : deviceCacheDir;
    const fname = slash >= 0 ? relPath.substring(slash + 1) : relPath;
    return { relPath, bytes, destDir, fname };
  });

  log(`Chapter prebake: uploading ${allFiles.length} cache files (pack mode)...`, '', 'PRE');
  let uploaded = 0;
  let failed = 0;
  let totalBytes = 0;

  let packResult = null;
  try {
    packResult = await uploadCacheFilesAsPackWs(
      items,
      (uploadedCount, totalCount, bytesSent) => {
        reportProgress(
          50 + Math.floor(45 * uploadedCount / totalCount),
          `uploading cache file ${uploadedCount}/${totalCount}`,
        );
      },
      () => (typeof operationCancelled !== 'undefined' && operationCancelled),
    );
  } catch (e) {
    packResult = { ok: false, error: e.message || String(e), uploaded: 0, totalBytes: 0 };
  }

  if (packResult && packResult.ok) {
    uploaded = packResult.uploaded;
    totalBytes = packResult.totalBytes;
  } else {
    // Pack mode failed. Fall back to per-file persistent-WS upload.
    log(`Pack upload failed (${packResult?.error || 'unknown'}); falling back to per-file persistent WS`,
        'warning', 'PRE');
    const fallback = await uploadCacheFilesPersistentWs(
      items,
      (uploadedCount, totalCount, bytesSent) => {
        reportProgress(
          50 + Math.floor(45 * uploadedCount / totalCount),
          `uploading cache file ${uploadedCount}/${totalCount}`,
        );
      },
      () => (typeof operationCancelled !== 'undefined' && operationCancelled),
    );
    uploaded = fallback.uploaded;
    failed = fallback.failed;
    totalBytes = fallback.totalBytes;
  }

  // 8. MEMFS cleanup lives in the worker now -- it rmrf's /out and unlinks
  //    inputs at the start of the NEXT 'run' message so per-book state
  //    doesn't pile up across a batch. Main thread has nothing to clean.

  const elapsedMs = Date.now() - startTime;
  reportProgress(100);
  log(`Chapter prebake done: ${uploaded}/${allFiles.length} files (${formatBytes(totalBytes)}) in ${(elapsedMs / 1000).toFixed(1)}s`,
      failed === 0 ? '' : 'warning', 'PRE-OK');
  return { hashId, uploaded, failed, totalBytes, elapsedMs };
}

async function convertEpubFile(file, progressCallback, options) {
  const startTime = Date.now();
  const originalSize = file.size;
  // CrumBLE 4.5.5: same overrideRenderInfo hook as prebakeChapters.
  // Caller (FilesPage.html optimize-selected + uploadFile flows) passes
  // the preflight modal's dryRun response so the .pxc bake locks layout
  // to the user's bake-time picks instead of the device's persisted
  // SETTINGS. Without this the user-picked SD font / size never made
  // it into the .pxc fontId; the BT pass and chapter prebake would
  // disagree about the bake's font fingerprint and on-device open
  // would reject one of them.
  const overrideRenderInfo = (options && options.overrideRenderInfo) || null;

  // Lazily load jszip (only needed for optimization).
  await ensureJSZip();

  // Initialize logging
  clearLog();
  showLog();
  log(`<strong>${file.name}</strong> <span class="log-detail">(${formatBytes(originalSize)})</span>`, '', 'INFO');
  log(`Quality: ${JPEG_QUALITY}% | Overlap: ${OVERLAP_PERCENT}% | Rotation: ${HANDEDNESS === 'right' ? 'CW' : 'CCW'} | Grayscale: ${ENABLE_GRAYSCALE ? 'ON' : 'OFF'}`, '', 'INFO');

  const zip = await JSZip.loadAsync(file);
  const renamed = {};
  zip.forEach(p => {
    const l = p.toLowerCase();
    if (l.match(/\.(png|gif|webp|bmp|jpeg)$/)) {
      renamed[p] = p.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
    }
  });

  // v18.9.9.381: cover-image detection. Ported from extractImagesForPreview
  // (line 36+). Was referenced later during cover-thumb baking at ~line 4527
  // but the variable only existed in the preview helper's scope, so any
  // convertEpubFile call from outside the image-picker path (e.g. Optimize &
  // Upload) threw "coverImagePath is not defined" and the whole conversion
  // aborted -- silently falling back to un-optimized upload.
  let coverImagePath = null;
  try {
    let opfPath = null;
    zip.forEach(p => { if (p.toLowerCase().endsWith('.opf')) opfPath = p; });
    if (opfPath) {
      const opfContent = await zip.files[opfPath].async('string');
      const opfDir = opfPath.includes('/') ? opfPath.substring(0, opfPath.lastIndexOf('/')) : '';
      let coverId = null;
      let m;
      if (m = opfContent.match(/<item[^>]+id=["']([^"']+)["'][^>]+properties="[^"]*cover-image[^"]*"/)) coverId = m[1];
      if (!coverId && (m = opfContent.match(/<item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id=["']([^"']+)["']/))) coverId = m[1];
      if (!coverId && (m = opfContent.match(/<meta\s+name=["']cover["']\s+content=["']([^"']+)["']/))) coverId = m[1];
      if (!coverId && (m = opfContent.match(/<meta\s+content=["']([^"']+)["']\s+name=["']cover["']/))) coverId = m[1];
      if (coverId) {
        const manifestRegex = /<item[^>]+id=["']([^"']+)["'][^>]+href=["']([^"']+)["'][^>]*>/gi;
        let match;
        while ((match = manifestRegex.exec(opfContent)) !== null) {
          if (match[1] === coverId) { coverImagePath = opfDir ? opfDir + '/' + match[2] : match[2]; break; }
        }
        if (!coverImagePath) {
          const manifestRegex2 = /<item[^>]+href=["']([^"']+)["'][^>]+id=["']([^"']+)["'][^>]*>/gi;
          while ((match = manifestRegex2.exec(opfContent)) !== null) {
            if (match[2] === coverId) { coverImagePath = opfDir ? opfDir + '/' + match[1] : match[1]; break; }
          }
        }
      }
    }
  } catch (e) {
    console.warn('convertEpubFile: cover-image detection failed:', e);
    // Non-fatal: cover-thumb bake below simply skips when coverImagePath is null.
  }

  const out = new JSZip();
  // Phase 5: chapter-prebake mode (the single "Pre-optimize for instant
  // reading" toggle in the modal) supersedes the old "Bluetooth-friendly
  // chapters" / store-XHTML-uncompressed toggle. When chapter prebake is
  // on the device loads sections from sections-prebake/*.bin and never
  // needs to inflate a DEFLATE chapter, so the 32 KB inflate window
  // bypass that the old toggle achieved is delivered for free. We always
  // DEFLATE chapter XHTML now -- smaller EPUB, no downside under the
  // prebake flow.
  const xhtmlFileOpts = { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false };
  // CrumBLE: fetch the device's reader render-info so we can pre-render each image
  // to its .pxc pixel cache at the exact dimensions the device will use. The
  // optimizer is served by the device, so this returns the live viewport.
  //
  // Two gates:
  //   1. User toggle: "Bake images for Bluetooth". Default on. Lets users
  //      opt out of the bake (and the size growth) when they don't read with
  //      a BT remote.
  //   2. Render-info fetch must succeed (fails when the optimizer runs
  //      standalone off-device).
  // Same checkbox now drives BT-pass .pxc baking AND the chapter prebake
  // pass (read separately in FilesPage.html's upload step). One toggle,
  // both off-device optimisations, single user mental model.
  const bakePxcEnabled = document.getElementById('optimize-for-device-checkbox')?.checked !== false;
  let renderInfo = null;
  if (bakePxcEnabled) {
    // FilesPage.html runs the preflight modal BEFORE calling
    // convertEpubFile so the user only sees it once even when both the
    // BT pass and the chapter prebake pass need the same locked-in
    // settings. CrumBLE 4.5.5: the modal no longer POSTs to SETTINGS --
    // it dryRuns the device and returns a bake-time-only renderInfo via
    // overrideRenderInfo. When the caller threads that through we use
    // it directly; legacy callers (or paths that skip the modal) fall
    // back to the live render-info fetch.
    if (overrideRenderInfo) {
      renderInfo = overrideRenderInfo;
    } else {
      try {
        const resp = await fetch('/api/reader-render-info');
        if (resp.ok) renderInfo = await resp.json();
      } catch (e) { renderInfo = null; }
    }
    if (!renderInfo || !(renderInfo.viewportWidth > 0) || !(renderInfo.viewportHeight > 0)) {
      renderInfo = null;
    } else {
      log(`Bluetooth image cache: baking .pxc for ${renderInfo.device} viewport ${renderInfo.viewportWidth}x${renderInfo.viewportHeight}`, '', 'INFO');
    }
  } else {
    log('Bluetooth image cache: skipped (user opted out)', '', 'INFO');
  }
  // CrumBLE: track how many .pxc files we actually wrote. We only emit the
  // manifest (META-INF/crumble-pxc.json) if at least one image got a .pxc --
  // otherwise the device would see a manifest but no cache files and prompt
  // the user about a layout it can't actually deliver.
  let pxcBakedCount = 0;

  const entries = Object.entries(zip.files);
  const splitImages = {};
  // v18.9.9.298: per-XHTML visible-text character counts, populated in the
  // xhtml loop below. Total emitted as META-INF/crumble-stats.json for
  // the device's stable-page-number path.
  const xhtmlCharCounts = {};
  const xhtmlFiles = {};
  let opfPath = null, opfContent = null;
  let mainIdentifier = null;

  // Write mimetype FIRST per EPUB OCF spec
  if (zip.files['mimetype']) {
    const mimetypeData = await zip.files['mimetype'].async('arraybuffer');
    out.file('mimetype', mimetypeData, { compression: 'STORE', createFolders: false });
  }

  // First pass: process images
  for (let i = 0; i < entries.length; i++) {
    if (operationCancelled) throw new Error('Cancelled by user');
    const [path, fileObj] = entries[i];
    if (fileObj.dir || path === 'mimetype') continue;
    const low = path.toLowerCase();

    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg)$/)) {
      const data = await fileObj.async('arraybuffer');
      const imageState = getImageState(path);

      let result;
      try {
        result = await processImage(data, imageState, path);
      } catch (imageError) {
        // Log error but continue with original image
        console.error(`Failed to process image ${path}:`, imageError);
        log(`Warning: Failed to process ${path.split('/').pop()}, using original`, 'warning', 'IMG-ERR');

        // Use original image data as fallback
        result = {
          parts: [{
            data: data,
            suffix: '',
            width: 0,
            height: 0,
            size: data.byteLength
          }],
          meta: {
            origW: 0,
            origH: 0,
            origSize: data.byteLength,
            wasSplit: false,
            rotated: false,
            finalW: 0,
            finalH: 0,
            finalSize: data.byteLength,
            imageState: imageState,
            processingError: true
          }
        };
      }

      const parts = result.parts;
      const meta = result.meta;

      const baseName = path.replace(/\.[^.]+$/, '');
      const newExt = '.jpg';

      // Log image processing
      const imgName = path.split('/').pop();
      const origFormat = path.split('.').pop();
      logImage(imgName, meta.origW, meta.origH, origFormat, meta.origSize, meta.finalW, meta.finalH, meta.finalSize, meta.wasSplit, meta.splitCount || 0, parts, meta.imageState || 0);

      if (parts.length === 1 && parts[0].suffix === '') {
        const newPath = renamed[path] || path.replace(/\.[^.]+$/, newExt);
        out.file(newPath, parts[0].data, { compression: 'STORE', createFolders: false });
        // CrumBLE: bake a .pxc pixel cache alongside (same path, .pxc ext) so the
        // device can render this image decoder-free over a Bluetooth remote. Only
        // the common single-image (non-split) case for now; split parts fall back
        // to on-device decode. A dimension mismatch on-device just falls back too.
        if (renderInfo) {
          try {
            const pxc = await bakePxc(parts[0].data, renderInfo.viewportWidth, renderInfo.viewportHeight);
            if (pxc) {
              out.file(newPath.replace(/\.[^.]+$/, '.pxc'), pxc, { compression: 'STORE', createFolders: false });
              pxcBakedCount++;
            }
          } catch (e) { /* skip this image's cache; not fatal */ }
        }
        // v18.9.9.291 Option A: bake 1-bit BMP cover thumbnails at
        // common device sizes. Device (Epub::convertCoverToThumbBmp)
        // checks META-INF/crumble-covers/thumb_WxH.bmp before decoding
        // any source PNG/JPG. Zero on-device PNG decode when hit --
        // skips the 32 KB DEFLATE inflate window entirely.
        if (path === coverImagePath) {
          for (const sz of COVER_THUMB_SIZES) {
            try {
              const bmp = await bakeCoverThumbBmp(data, sz.w, sz.h);
              if (bmp) {
                const bmpPath = `META-INF/crumble-covers/thumb_${sz.w}x${sz.h}.bmp`;
                out.file(bmpPath, bmp, { compression: 'STORE', createFolders: true });
              }
            } catch (e) {
              console.warn(`cover thumb bake failed for ${sz.w}x${sz.h}:`, e);
            }
          }
          log(`Cover thumbnails: pre-baked at ${COVER_THUMB_SIZES.length} sizes`, '', 'INFO');
        }
      } else {
        // Store with full path for collision prevention, but also keep original filename
        const origName = path.split('/').pop();
        const origDir = path.includes('/') ? path.substring(0, path.lastIndexOf('/')) : '';

        // Key by full path to avoid collisions
        splitImages[path] = {
          origName: origName,
          origDir: origDir,
          parts: []
        };

        for (const part of parts) {
          const partName = baseName.split('/').pop() + part.suffix + newExt;
          const partPath = (path.includes('/') ? path.substring(0, path.lastIndexOf('/') + 1) : '') + partName;
          out.file(partPath, part.data, { compression: 'STORE', createFolders: false });
          // Store metadata for XHTML/OPF updates
          splitImages[path].parts.push({
            path: partPath,
            imgName: partName,
            id: baseName.split('/').pop() + part.suffix,
            suffix: part.suffix
          });
        }
      }
    } else if (low.match(/\.(xhtml|html|htm)$/)) {
      xhtmlFiles[path] = await safeReadText(fileObj);
    } else if (low.endsWith('.opf')) {
      opfPath = path;
      opfContent = await safeReadText(fileObj);
    }

    if (progressCallback) progressCallback((i / entries.length) * 60);
  }

  // Second pass: update XHTML using DOMParser
  for (const [xhtmlPath, content] of Object.entries(xhtmlFiles)) {
    if (operationCancelled) throw new Error('Cancelled by user');
    let t = content;
    const r = fixSvgCover(t);
    if (r.fixed) { t = r.c; logFix('SVG cover', xhtmlPath.split('/').pop()); }

    const r2 = fixSvgWrappedImages(t);
    if (r2.fixed) { t = r2.c; logFix(`SVG images (${r2.count})`, xhtmlPath.split('/').pop()); }

    // Use DOMParser for all img modifications: remove width/height and handle split images
    try {
      const whitespaceGuard = protectWhitespaceOnlyTextNodes(t);
      const parser = new DOMParser();
      const doc = parser.parseFromString(whitespaceGuard.content, 'application/xhtml+xml');
      const parseError = doc.querySelector('parsererror');

      if (!parseError) {
        let modified = false;

        // Remove width/height attributes from ALL img tags (dimensions may have changed)
        // This prevents CrossInk and other readers from using wrong dimensions
        const allImgElements = doc.querySelectorAll('img');
        for (const img of allImgElements) {
          if (img.hasAttribute('width')) { img.removeAttribute('width'); modified = true; }
          if (img.hasAttribute('height')) { img.removeAttribute('height'); modified = true; }

          const src = img.getAttribute('src');
          if (src) {
            const decodedSrc = decodeHref(src);
            const resolvedSrc = resolvePath(xhtmlPath, decodedSrc);

            const match = Object.entries(renamed).find(([oldPath]) => resolvedSrc === oldPath);

            if (match) {
              const [oldPath, newPath] = match;
              img.setAttribute('src', decodedSrc.replace(oldPath.split('/').pop(), newPath.split('/').pop()));
              modified = true;
            }
          }
        }

        // Handle split images with path collision prevention
        if (Object.keys(splitImages).length > 0) {
          // Get XHTML directory for resolving relative paths
          const xhtmlDir = xhtmlPath.includes('/') ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf('/')) : '';
          const rootFolders = ['ops', 'oebps', 'epub', 'content'];

          for (const [fullPath, splitInfo] of Object.entries(splitImages)) {
            const origName = splitInfo.origName;
            const origDir = splitInfo.origDir;
            const parts = splitInfo.parts;
            const newName = origName.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');

            // Extract immediate parent directory for collision prevention
            const splitDirParts = origDir.split('/').filter(p => p);
            const lastDir = splitDirParts.length > 0 ? splitDirParts[splitDirParts.length - 1].toLowerCase() : null;
            const immediateParent = (lastDir && !rootFolders.includes(lastDir)) ? splitDirParts[splitDirParts.length - 1] : null;

            // Get XHTML's parent directory parts for relative path resolution
            const xhtmlDirParts = xhtmlDir.split('/').filter(p => p);

            // Find all img elements
            const allImgs = doc.querySelectorAll('img');
            const matchingImgs = [];

            for (const img of allImgs) {
              const src = img.getAttribute('src') || '';
              const srcParts = src.split('/').filter(p => p && p !== '..' && p !== '.');
              const srcName = srcParts.pop() || '';

              // Check filename match
              if (srcName !== origName && srcName !== newName) continue;

              // Path collision prevention with root folder handling
              if (immediateParent) {
                // Image is in a specific subfolder (not root like OPS/OEBPS)
                if (srcParts.length === 0) {
                  // src has no path - check if XHTML is in same folder as image
                  const xhtmlLastDir = xhtmlDirParts.length > 0 ? xhtmlDirParts[xhtmlDirParts.length - 1] : null;
                  if (xhtmlLastDir !== immediateParent) continue;
                } else {
                  // src has path - verify parent directory matches
                  if (srcParts[srcParts.length - 1] !== immediateParent) continue;
                }
              } else {
                // Image is in root folder (like OEBPS/cover.jpg)
                // Only match if src has NO subfolder path OR points to root folder
                if (srcParts.length > 0) {
                  const srcLastDir = srcParts[srcParts.length - 1].toLowerCase();
                  if (!rootFolders.includes(srcLastDir)) continue;
                }
              }

              matchingImgs.push(img);
            }

            // Process each matching img — Pro's strip+inject approach
            for (const img of matchingImgs) {
              const src = img.getAttribute('src') || '';

              // Part 1: update src in-place, strip original sizing
              img.setAttribute('src', src.replace(origName, parts[0].imgName).replace(newName, parts[0].imgName));

              if (parts.length > 1) {
                // Strip original width/height/class that were sized for the unsplit image
                img.removeAttribute('width');
                img.removeAttribute('height');
                img.removeAttribute('class');
                img.setAttribute('style', 'max-width:100%;height:auto');

                // Neutralize container height constraints that were sized for the original
                let container = img.parentElement;
                const safeContainers = ['div', 'p', 'figure', 'aside', 'section', 'body'];
                while (container && !safeContainers.includes(container.tagName.toLowerCase())) container = container.parentElement;
                const insertTarget = container || img.parentElement;
                // Strip constraining classes/styles from container — they were for the unsplit image
                if (insertTarget && insertTarget.tagName.toLowerCase() !== 'body') {
                  insertTarget.removeAttribute('class');
                  insertTarget.removeAttribute('style');
                }
                const insertParent = insertTarget.parentElement;
                const insertRef = insertTarget.nextSibling;
                const ns = doc.documentElement.namespaceURI || 'http://www.w3.org/1999/xhtml';

                // Insert new minimal wrappers for parts 2+ in reading order
                for (let pi = 1; pi < parts.length; pi++) {
                  const wrapper = doc.createElementNS(ns, 'div');
                  const newImg = doc.createElementNS(ns, 'img');
                  const partSrc = src.replace(origName, parts[pi].imgName).replace(newName, parts[pi].imgName);
                  newImg.setAttribute('src', partSrc);
                  newImg.setAttribute('alt', '');
                  newImg.setAttribute('style', 'max-width:100%;height:auto');
                  wrapper.appendChild(newImg);
                  if (insertRef) insertParent.insertBefore(wrapper, insertRef);
                  else insertParent.appendChild(wrapper);
                }
              }
              modified = true;
            }
          }
        }

        // Only serialize if we made changes
        if (modified) {
          t = whitespaceGuard.restore(safeSerialize(doc, whitespaceGuard.content));
        }
      }
    } catch (e) {
      console.warn('DOMParser error for', xhtmlPath, e.message);
    }

    // Inject universal image constraint — prevents overflow on e-ink displays
    if (t.includes('</head>')) {
      t = t.replace('</head>', DEFENSIVE_STYLE + '</head>');
    }

    // Safety net: guarantee every renamed image's reference in the chapter text
    // points at its new .jpg filename. The DOMParser pass above only rewrites
    // <img src> when the XHTML parses as strict application/xhtml+xml AND
    // resolvePath() matches the renamed key exactly; real-world EPUBs routinely
    // miss one of those, which left the file renamed to .jpg while the chapter
    // still said .png -- the reader then can't extract the image and renders
    // "[Image: alt]" for every picture. This text-based pass (same approach as
    // the OPF rewrite below) doesn't depend on either condition and is a no-op
    // when the DOMParser pass already did the rename. Split images expand into
    // multiple parts via the DOMParser pass, so their original name must NOT be
    // blindly renamed here.
    let renameFallbackCount = 0;
    for (const [oldPath, newPath] of Object.entries(renamed)) {
      if (splitImages[oldPath]) continue;
      const oldFile = oldPath.split('/').pop();
      const newFile = newPath.split('/').pop();
      if (oldFile && newFile && oldFile !== newFile && t.includes(oldFile)) {
        t = t.split(oldFile).join(newFile);
        renameFallbackCount++;
      }
    }
    if (renameFallbackCount > 0) {
      logFix('Image refs', `${renameFallbackCount} fixed in ${xhtmlPath.split('/').pop()}`);
    }

    out.file(xhtmlPath, t, xhtmlFileOpts);

    // v18.9.9.298: char-count for real Stable Page Numbers. Strip
    // scripts/styles/tags/entities and count the actual visible text
    // per XHTML file. Device reads META-INF/crumble-stats.json below
    // and prefers `totalChars` over its inflated-byte-size approximation
    // (which over-counts due to HTML/CSS/base64 markup).
    try {
      let visibleText = t
          .replace(/<script\b[^<]*(?:(?!<\/script>)<[^<]*)*<\/script>/gi, '')
          .replace(/<style\b[^<]*(?:(?!<\/style>)<[^<]*)*<\/style>/gi, '')
          .replace(/<[^>]+>/g, '')
          .replace(/&nbsp;/g, ' ')
          .replace(/&amp;/g, '&')
          .replace(/&lt;/g, '<')
          .replace(/&gt;/g, '>')
          .replace(/&quot;/g, '"')
          .replace(/&apos;/g, "'")
          .replace(/&#(\d+);/g, (m, code) => String.fromCharCode(parseInt(code, 10)))
          .replace(/&#x([0-9a-fA-F]+);/g, (m, code) => String.fromCharCode(parseInt(code, 16)))
          .replace(/\s+/g, ' ')
          .trim();
      // Use Array.from spread to count code points (surrogate-pair safe --
      // .length would double-count astral-plane chars like most emoji).
      const chars = Array.from(visibleText).length;
      xhtmlCharCounts[xhtmlPath] = chars;
    } catch (e) {
      console.warn('char-count failed for', xhtmlPath, e);
    }
  }

  // Extract main identifier from OPF using DOMParser with regex fallback
  if (opfContent) {
    mainIdentifier = extractIdentifier(opfContent);
  }

  // Third pass: update OPF using fixOPF (DOMParser with regex fallback)
  if (opfContent) {
    let t = opfContent;
    for (const [o, n] of Object.entries(renamed)) {
      t = t.split(o.split('/').pop()).join(n.split('/').pop());
    }
    const opfDir = opfPath.includes('/') ? opfPath.substring(0, opfPath.lastIndexOf('/')) : '';
    t = fixOPF(t, opfContent, opfDir, splitImages);
    if (t !== opfContent) logFix('OPF', 'manifest updated');
    out.file(opfPath, t, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  // Copy remaining files
  for (const [path, fileObj] of entries) {
    if (operationCancelled) throw new Error('Cancelled by user');
    if (fileObj.dir || path === 'mimetype') continue;
    const low = path.toLowerCase();
    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg)$/) || low.match(/\.(xhtml|html|htm)$/) || low.endsWith('.opf')) continue;

    let data = await fileObj.async('arraybuffer');
    if (low.endsWith('.css')) {
      let t = await safeReadText(fileObj);
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split('/').pop()).join(n.split('/').pop());
      }
      data = new TextEncoder().encode(t);
    } else if (low.endsWith('.ncx')) {
      let t = await safeReadText(fileObj);
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split('/').pop()).join(n.split('/').pop());
      }
      const oldT = t;
      t = syncNCXIdentifier(t, mainIdentifier);
      if (t !== oldT) logFix('NCX identifier', 'Synced with OPF');
      data = new TextEncoder().encode(t);
    }
    out.file(path, data, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  // CrumBLE: emit the .pxc manifest only if we actually wrote pxc files. The
  // device reads this on book open and, when the user connects a Bluetooth
  // remote, compares the four viewport-affecting fields (fontId, orientation,
  // screenMargin, imageRendering) to current SETTINGS. On mismatch it asks
  // the user to switch to the baked layout so the .pxc images render. The
  // schema mirrors /api/reader-render-info exactly -- forward-compatibility
  // is easier if the manifest is just a snapshot of that contract.
  //
  // CrumBLE 4.5.6: validate manifest.fontId against the SD font's true ID
  // before writing. The override in showPreflightAndFontImport (L2495-2501)
  // fixes merged.fontId only for the modal path, but renderInfo here can
  // arrive via /api/reader-render-info (older bake invocations) or via
  // overrideRenderInfo whose fontId wasn't patched (e.g. when the modal
  // skipped the override block because sdFontPickedPointSize wasn't yet
  // resolved). Sections always bake with the SD font's true fontId because
  // the CLI computes it from the .cpfont bytes loaded into MEMFS (L3654+).
  // If the manifest's fontId disagrees with sections', the device sees a
  // PxcManifest mismatch even when SETTINGS already aligns with the prebake.
  // That triggers a duplicate "use prepared layout?" prompt during BT quick
  // connect; if the user accepts, SETTINGS get rewritten with the manifest's
  // (wrong) fontId, sections refuse to install on fingerprint check, the
  // SD font subset fallback OOMs, and every glyph renders as `?`. Validate
  // here so the manifest always carries the same fontId the sections do.
  let manifestFontId = renderInfo.fontId;
  if (renderInfo && renderInfo.sdFontFamilyName && renderInfo.sdFontFamilyName.length > 0
      && (renderInfo.sdFontPickedPointSize | 0) > 0) {
    try {
      const verResp = await fetch(
        `/api/fonts/version?family=${encodeURIComponent(renderInfo.sdFontFamilyName)}&size=${renderInfo.sdFontPickedPointSize | 0}`,
        { cache: 'no-store' });
      if (verResp.ok) {
        const fontVer = await verResp.json();
        if (typeof fontVer.fontId === 'number' && fontVer.fontId !== 0
            && fontVer.fontId !== manifestFontId) {
          log(`Manifest fontId fixup: ${manifestFontId} -> ${fontVer.fontId} (from /api/fonts/version for ${renderInfo.sdFontFamilyName}@${renderInfo.sdFontPickedPointSize | 0}pt)`, '', 'INFO');
          manifestFontId = fontVer.fontId | 0;
        }
      }
    } catch (_) { /* offline or older firmware: keep renderInfo.fontId */ }
  }

  if (renderInfo && pxcBakedCount > 0) {
    const manifest = {
      v: 1,
      device: renderInfo.device || 'X4',
      fitVersion: renderInfo.fitVersion || 1,
      orientation: renderInfo.orientation,
      screenMargin: renderInfo.screenMargin,
      imageRendering: renderInfo.imageRendering,
      fontId: manifestFontId,
      // CrumBLE: raw font fields for human-readable display on the device.
      // Optional in older builds -- the device parser tolerates missing keys.
      fontFamily: renderInfo.fontFamily,
      fontSize: renderInfo.fontSize,
      sdFontSizeRange: renderInfo.sdFontSizeRange,
      sdFontFamilyName: renderInfo.sdFontFamilyName,
      screenWidth: renderInfo.screenWidth,
      screenHeight: renderInfo.screenHeight,
      viewportWidth: renderInfo.viewportWidth,
      viewportHeight: renderInfo.viewportHeight,
      emSize: renderInfo.emSize,
      pxcCount: pxcBakedCount,
    };
    out.file('META-INF/crumble-pxc.json',
             new TextEncoder().encode(JSON.stringify(manifest)),
             { compression: 'STORE', createFolders: false });
    log(`Wrote .pxc manifest for ${pxcBakedCount} image(s)`, '', 'INFO');
  }

  // v18.9.9.298: emit crumble-stats.json even without pxc bake -- the
  // char-count is independent of image processing. Device reads
  // META-INF/crumble-stats.json for real Stable Page Numbers; missing
  // manifest falls back to the byte-size approximation as before.
  {
    let totalChars = 0;
    for (const n of Object.values(xhtmlCharCounts)) totalChars += n;
    if (totalChars > 0) {
      const statsManifest = { v: 1, totalChars };
      out.file('META-INF/crumble-stats.json',
               new TextEncoder().encode(JSON.stringify(statsManifest)),
               { compression: 'STORE', createFolders: false });
      log(`Wrote stats manifest: ${totalChars} chars across ${Object.keys(xhtmlCharCounts).length} XHTML files`, '', 'INFO');
    }
  }

  if (progressCallback) progressCallback(100);

  // Generate final blob
  const newBlob = await out.generateAsync({ type: 'blob', mimeType: 'application/epub+zip' });
  const newSize = newBlob.size;
  const timeElapsed = (Date.now() - startTime) / 1000;

  // Log completion
  log('Conversion complete!', 'success', 'DONE');
  logSummary(originalSize, newSize, timeElapsed);

  // Auto-export only if NOT in batch mode (batch mode exports at the end)
  if (!isBatchMode && exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, false); // isBatch = false for single file
    }, 100);
  }

  return newBlob;
}
