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
      '<h2 style="margin:0 0 6px;font-size:20px;color:#222">Lock in reader settings?</h2>' +
      '<p style="margin:0 0 16px;color:#555;font-size:14px;line-height:1.5">' +
      'Optimizing this book takes up to a few minutes (depending on length and images) ' +
      'and locks the prepared layout to specific reader settings. Verify these are the ' +
      'settings you read with. Anything you change here updates the device too.</p>';
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
    makeBool('Extra paragraph spacing', 'extraParagraphSpacing', renderInfo.extraParagraphSpacing | 0);
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
    overlay.onclick = (e) => { if (e.target === overlay) { close(); reject(new Error('Cancelled by user')); } };

    confirm.onclick = async () => {
      const updates = {};
      const dirtyKeys = [];
      form.querySelectorAll('[data-key]').forEach(el => {
        const key = el.dataset.key;
        const cur = el.dataset.current;
        const val = (el.type === 'checkbox') ? (el.checked ? '1' : '0') : el.value;
        if (val !== cur) {
          dirtyKeys.push(key);
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
        }
      });
      if (dirtyKeys.length === 0) {
        // Same SD-pt-size attachment as the post-save branch -- the user
        // may have opened the modal on an SD font, confirmed without
        // changing anything, and we still need to tell the bake what
        // point size .cpfont to load.
        const familyVal = familySel.value;
        const sizeVal = sizeSel.value;
        if (familyVal.startsWith('sd:')) {
          const sdName = familyVal.substring(3);
          const family = SD_FONT_FAMILIES[sdName];
          if (family && family.sizes.length > 0) {
            const sorted = family.sizes.slice().sort((a, b) => (a | 0) - (b | 0));
            const idx = Math.min(sorted.length - 1, Math.max(0, sizeVal | 0));
            renderInfo.sdFontPickedPointSize = sorted[idx];
          }
        }
        close();
        resolve(renderInfo);
        return;
      }
      confirm.disabled = true;
      confirm.textContent = 'Saving settings...';
      try {
        const resp = await fetch('/api/save-reader-settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(updates),
        });
        if (!resp.ok) {
          const errText = await resp.text();
          throw new Error('Save failed: ' + errText);
        }
        const fresh = await fetch('/api/reader-render-info').then(r => r.json());
        // CrumBLE 4.2: surface the SD-card point size the user actually
        // picked, so the bake step downstream knows what size .cpfont to
        // ship to WASM. The device's stored fontSize is an enum index
        // that doesn't carry the SD font's real point size; we look up
        // the picked size's pt-value from the family.sizes array and
        // attach it as a separate field on renderInfo.
        const familyVal = familySel.value;
        const sizeVal = sizeSel.value;
        if (familyVal.startsWith('sd:')) {
          const sdName = familyVal.substring(3);
          const family = SD_FONT_FAMILIES[sdName];
          if (family && family.sizes.length > 0) {
            const sorted = family.sizes.slice().sort((a, b) => (a | 0) - (b | 0));
            const idx = Math.min(sorted.length - 1, Math.max(0, sizeVal | 0));
            fresh.sdFontPickedPointSize = sorted[idx];
          }
        }
        close();
        resolve(fresh);
      } catch (e) {
        confirm.disabled = false;
        confirm.textContent = 'Looks good, optimize';
        alert('Could not save settings: ' + (e.message || e));
      }
    };
  });
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

// Get a ready Module instance. The factory is invoked once and the resulting
// Module is reused across runs (EXIT_RUNTIME=0 keeps MEMFS + libc alive
// between callMain invocations). This means we pay the ~850 KB WASM
// download + instantiation cost once per session, not per book.
async function loadCrumblePrebakeModule() {
  if (crumblePrebakeModulePromise) return crumblePrebakeModulePromise;
  crumblePrebakeModulePromise = (async () => {
    const factory = await loadCrumblePrebakeFactory();
    return await factory({
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
      },
      printErr: (msg) => {
        crumblePrebakeOutputBuf.push(msg);
        try { console.error('[prebake-err]', msg); } catch (e) {}
        try { log(`[prebake-err] ${msg}`, 'warning', 'PRE-ERR'); } catch (e) {}
      },
    });
  })();
  return crumblePrebakeModulePromise;
}

// Upload a single byte buffer to /upload?path=<destDir> with filename <name>.
// Returns the HTTP status code; throws on network failure.
async function uploadOneToDevice(destDir, name, bytes) {
  const formData = new FormData();
  // The handler infers the filename from the upload's part name; we set it
  // explicitly so the destination basename matches what we generated.
  formData.append('file', new Blob([bytes]), name);
  const resp = await fetch(`/upload?path=${encodeURIComponent(destDir)}`, {
    method: 'POST',
    body: formData,
  });
  return resp;
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
async function prebakeChapters(epubBlob, deviceFilePath, progressCallback) {
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
  log('Chapter prebake: fetching device render settings...', '', 'PRE');
  const riResp = await fetch('/api/reader-render-info');
  if (!riResp.ok) {
    throw new Error(`render-info fetch failed: HTTP ${riResp.status}`);
  }
  const renderInfo = await riResp.json();
  if (!(renderInfo.fitVersion >= 2)) {
    throw new Error(`device reports fitVersion=${renderInfo.fitVersion}; chapter prebake needs >=2 (update firmware)`);
  }
  reportProgress(5, 'loading WASM module...');

  // 2. Boot the WASM module (downloads ~850 KB gz on first call).
  log('Chapter prebake: loading WASM module...', '', 'PRE');
  const Module = await loadCrumblePrebakeModule();
  reportProgress(15, 'building chapter index...');

  // 3. Stage the EPUB + settings file into MEMFS so the CLI sees them as
  //    regular files. The CLI's --device-path argument controls the cache
  //    hash so we use the actual device-side path (e.g. "/foo.epub"),
  //    matching what the device computed when it indexed this book.
  log('Chapter prebake: staging input into WASM filesystem...', '', 'PRE');
  const epubBytes = new Uint8Array(await epubBlob.arrayBuffer());
  const inputPath = '/input.epub';
  const settingsPath = '/settings.json';
  const outDir = '/out';
  const sdFontPath = '/sd_font.cpfont';
  try { Module.FS.unlink(inputPath); } catch (e) {}
  try { Module.FS.unlink(settingsPath); } catch (e) {}
  try { Module.FS.unlink(outDir); } catch (e) {}
  try { Module.FS.unlink(sdFontPath); } catch (e) {}
  Module.FS.writeFile(inputPath, epubBytes);
  Module.FS.writeFile(settingsPath, JSON.stringify(renderInfo));
  Module.FS.mkdir(outDir);

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
      try {
        const fontResp = await fetch(`/api/fonts/file?family=${encodeURIComponent(fname)}&size=${pt}`);
        if (!fontResp.ok) {
          throw new Error(`/api/fonts/file ${fontResp.status}`);
        }
        const fontBytes = new Uint8Array(await fontResp.arrayBuffer());
        Module.FS.writeFile(sdFontPath, fontBytes);
        cliArgs.unshift('--sd-font-path', sdFontPath,
                        '--sd-font-family', fname,
                        '--sd-font-size', String(pt));
        log(`SD font passed to WASM: path=${sdFontPath} family=${fname} pt=${pt}`, '', 'PRE');
      } catch (e) {
        log(`SD font fetch failed (${e.message || e}) -- bake will use default font; prebake will not load cleanly`,
            '', 'WARN');
      }
    } else {
      log(`SD font: no point size resolved -- bake will use default font`, '', 'WARN');
    }
  }
  log(`Chapter prebake: callMain(${JSON.stringify(cliArgs)})`, '', 'PRE');
  // Clear the print/printErr capture buffer right before we run so any
  // earlier session output doesn't pollute this run's diagnostics.
  crumblePrebakeOutputBuf.length = 0;
  const rc = Module.callMain(cliArgs);
  if (rc !== 0) {
    // Surface whatever the CLI actually said on its way out. Without this
    // the thrown error is just "exit code N" with no clue what was wrong.
    const captured = crumblePrebakeOutputBuf.slice(-12).join('\n');
    const detail = captured ? `\n--- CLI output ---\n${captured}` : '';
    throw new Error(`prebake CLI exited with code ${rc}${detail}`);
  }
  reportProgress(50, 'uploading cache files...');

  // 5. Walk MEMFS to find the produced cache directory. Layout:
  //    /out/.crosspoint/epub_<hash>/{book.bin,sections-prebake/*.bin,...}
  const crosspointDir = `${outDir}/.crosspoint`;
  const cacheRoots = Module.FS.readdir(crosspointDir).filter(
    (n) => n.startsWith('epub_'),
  );
  if (cacheRoots.length !== 1) {
    throw new Error(`expected exactly one epub_* cache dir, found ${cacheRoots.length}`);
  }
  const hashId = cacheRoots[0]; // "epub_<hash>"
  const memCacheDir = `${crosspointDir}/${hashId}`;
  const deviceCacheDir = `/.crosspoint/${hashId}`;

  // 6. Make sure the destination dirs exist on device. mkdir for the cache
  //    dir is idempotent-ish (400 if exists, fine). sections-prebake/
  //    too. Without these, /upload's openFileForWrite would fail because
  //    the handler doesn't auto-create parents.
  await ensureRemoteDir('/.crosspoint', hashId);
  await ensureRemoteDir(deviceCacheDir, 'sections-prebake');

  // 7. Enumerate output files and upload each one. Order: top-level files
  //    first (book.bin, manifest, css), then sections in numeric order.
  //    This means the manifest lands quickly and the device's switch-back
  //    prompt has something to check before all sections finish.
  function* walkMemfs(dir, relPrefix) {
    for (const entry of Module.FS.readdir(dir)) {
      if (entry === '.' || entry === '..') continue;
      const full = `${dir}/${entry}`;
      const stat = Module.FS.stat(full);
      const rel = relPrefix ? `${relPrefix}/${entry}` : entry;
      if (Module.FS.isDir(stat.mode)) {
        yield* walkMemfs(full, rel);
      } else {
        yield { memPath: full, relPath: rel };
      }
    }
  }
  const allFiles = Array.from(walkMemfs(memCacheDir, ''));
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

  log(`Chapter prebake: uploading ${allFiles.length} cache files...`, '', 'PRE');
  let uploaded = 0;
  let failed = 0;
  let totalBytes = 0;
  for (let i = 0; i < allFiles.length; i++) {
    const { memPath, relPath } = allFiles[i];
    const bytes = Module.FS.readFile(memPath);
    totalBytes += bytes.length;
    // Destination dir is deviceCacheDir + parent-of-relPath.
    const slash = relPath.lastIndexOf('/');
    const destSubdir = slash >= 0 ? relPath.substring(0, slash) : '';
    const destDir = destSubdir ? `${deviceCacheDir}/${destSubdir}` : deviceCacheDir;
    const fname = slash >= 0 ? relPath.substring(slash + 1) : relPath;
    try {
      const resp = await uploadOneToDevice(destDir, fname, bytes);
      if (resp.ok) {
        uploaded++;
      } else {
        failed++;
        const errText = await resp.text().catch(() => '');
        log(`Upload failed ${relPath}: HTTP ${resp.status} ${errText}`.trim(), 'warning', 'PRE-FAIL');
      }
    } catch (e) {
      failed++;
      log(`Upload error ${relPath}: ${e.message}`, 'warning', 'PRE-FAIL');
    }
    // 50% -> 95% across the upload phase. Include the running file
    // count in the status text so the user can see SOMETHING moves even
    // when each upload nudges the bar by only a fraction of a percent
    // (~0.25% per file for the typical ~180-file cache).
    reportProgress(
      50 + Math.floor(45 * (i + 1) / allFiles.length),
      `uploading cache file ${i + 1}/${allFiles.length}`,
    );
  }

  // 8. Clean MEMFS so the next book in a batch doesn't pile up entries.
  //    EXIT_RUNTIME=0 keeps the Module alive, but we don't want to leak
  //    ~MB of EPUB / cache bytes per book across batch runs.
  function rmRf(path) {
    try {
      const stat = Module.FS.stat(path);
      if (Module.FS.isDir(stat.mode)) {
        for (const entry of Module.FS.readdir(path)) {
          if (entry === '.' || entry === '..') continue;
          rmRf(`${path}/${entry}`);
        }
        Module.FS.rmdir(path);
      } else {
        Module.FS.unlink(path);
      }
    } catch (e) {}
  }
  rmRf(inputPath);
  rmRf(settingsPath);
  rmRf(outDir);

  const elapsedMs = Date.now() - startTime;
  reportProgress(100);
  log(`Chapter prebake done: ${uploaded}/${allFiles.length} files (${formatBytes(totalBytes)}) in ${(elapsedMs / 1000).toFixed(1)}s`,
      failed === 0 ? '' : 'warning', 'PRE-OK');
  return { hashId, uploaded, failed, totalBytes, elapsedMs };
}

async function convertEpubFile(file, progressCallback) {
  const startTime = Date.now();
  const originalSize = file.size;

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
    // settings. By the time we get here, SETTINGS on the device already
    // reflects the user's edits (the modal POSTed them) -- we just need
    // a fresh render-info snapshot to drive the .pxc bake.
    try {
      const resp = await fetch('/api/reader-render-info');
      if (resp.ok) renderInfo = await resp.json();
    } catch (e) { renderInfo = null; }
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
  if (renderInfo && pxcBakedCount > 0) {
    const manifest = {
      v: 1,
      device: renderInfo.device || 'X4',
      fitVersion: renderInfo.fitVersion || 1,
      orientation: renderInfo.orientation,
      screenMargin: renderInfo.screenMargin,
      imageRendering: renderInfo.imageRendering,
      fontId: renderInfo.fontId,
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
