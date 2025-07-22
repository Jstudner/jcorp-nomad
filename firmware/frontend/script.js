// Fonctions communes

function copyLink() {
    var copyText = document.getElementById("copyLink");
    copyText.select();
    document.execCommand("copy");
    alert("Link copied to clipboard!");
}

function adjustForDevice() {
    if (window.innerWidth > 600) {
        document.getElementById('instructionHeader').textContent = "You're on a PC!";
        document.getElementById('instructionText').innerHTML = 
            "Use the button below to go directly to the menu, or copy and paste the link in a new tab.";
        document.getElementById('instructionList').innerHTML = `
            <li>Copy the link below:</li>
            <li>
                <input type="text" value="192.168.4.1/menu.html" id="copyLink" readonly>
                <button id="copyLinkButton" onclick="copyLink()">Copy Link</button>
            </li>
            <li>Open the link in a new browser tab.</li>
            <li>Or click the "Go to Menu" button below.</li>
        `;
        document.getElementById('goToMenuButton').style.display = 'block'; 
    } else {
        document.getElementById('goToMenuButton').style.display = 'none';
    }
}

// Initialisation
window.addEventListener('resize', adjustForDevice);
adjustForDevice();

// Gestion des livres
let books = [], filtered = [], currentBook = null;
const booksKey = 'nomad-books-size';
let currentSize = localStorage.getItem(booksKey) || 'medium';
let debounceTimer = null;

function setViewSize(size) {
    currentSize = size;
    viewButtons.forEach(btn => btn.classList.toggle('active', btn.dataset.size === size));
    gridElement.className = `media-grid ${size}`;
    localStorage.setItem(booksKey, size);
}

async function init() {
    try {
        const res = await fetch('/media.json');
        const json = await res.json();
        const raw = json.books || json.Books || [];
        books = raw.map(b => ({
            name: b.name || b.title || 'Unknown',
            cover: b.cover || b.thumbnail || 'placeholder.jpg',
            path: b.file || b.path || b.url || ''
        })).filter(b => b.path);
        applyFilter('');
    } catch (err) {
        console.error('Media load error', err);
        gridElement.innerHTML = '<p style="grid-column:1/-1;text-align:center;color:var(--primary-dark);">Unable to load content.</p>';
    }
}

function applyFilter(term) {
    const lower = term.trim().toLowerCase();
    filtered = !lower ? [...books] : books.filter(b => b.name.toLowerCase().includes(lower));
    applySort(sortSelect.value);
}

function applySort(mode) {
    if (mode === 'nameDesc') {
        filtered.sort((a, b) => b.name.localeCompare(a.name));
    } else {
        filtered.sort((a, b) => a.name.localeCompare(b.name));
    }
    renderGrid();
}

function renderGrid() {
    gridElement.className = `media-grid ${currentSize}`;
    const frag = document.createDocumentFragment();
    filtered.forEach(item => {
        const card = document.createElement('div');
        card.className = 'media-card';
        card.innerHTML = `<img class="media-thumb" loading="lazy" src="${item.cover}" alt="${item.name}">`;
        card.addEventListener('click', () => showBookOptions(item));
        frag.appendChild(card);
    });
    gridElement.innerHTML = '';
    gridElement.appendChild(frag);
}

function showBookOptions(book) {
    currentBook = book;
    document.getElementById('modalTitle').textContent = `What would you like to do with "${book.name}"?`;
    modal.style.display = 'flex';
}

function closeModal(evt) {
    if (evt.target === modal) {
        modal.style.display = 'none';
    }
}

function openBook(book) {
    let url = book.path;
    if (fileExt(url) === '.epub') {
        url = `/reader.html?file=${encodeURIComponent(url)}`;
    }
    window.open(url, '_blank');
}

function downloadBook(book) {
    const a = document.createElement('a');
    a.href = book.path;
    a.download = book.name;
    a.click();
}

// Gestion des événements
document.addEventListener('DOMContentLoaded', function() {
    viewButtons.forEach(btn => btn.addEventListener('click', () => setViewSize(btn.dataset.size)));
    searchInput.addEventListener('input', e => {
        clearTimeout(debounceTimer);
        debounceTimer = setTimeout(() => applyFilter(e.target.value), 150);
    });
    sortSelect.addEventListener('change', () => applySort(sortSelect.value));
    init();
});

// Gestion de l'administration
document.addEventListener('DOMContentLoaded', function() {
    // Elements
    const colorPicker = document.getElementById('led-color');
    const modeButtons = {
        off: document.getElementById('mode-off'),
        solid: document.getElementById('mode-solid'),
        rainbow: document.getElementById('mode-rainbow'),
        pulse: document.getElementById('mode-pulse')
    };
    let ledMode = 'off';

    // Gestion des messages globaux
    function showGlobal(msg) {
        gmsg.textContent = msg;
        gbar.value = 0;
    }

    function updateGlobal(p) {
        gbar.value = p;
        gmsg.textContent = Math.round(p) + '%';
    }

    function hideGlobal() {
        setTimeout(() => {
            gmsg.textContent = '';
            gfile.textContent = '';
            gbar.value = 0;
        }, 1000);
    }

    // Gestion des fichiers
    async function fetchSD() {
        try {
            const res = await fetch('/sdinfo');
            const { total, used } = await res.json();
            sdTotal.textContent = `Total: ${(total/1e9).toFixed(2)} GB`;
            sdUsed.textContent = `Used: ${(used/1e9).toFixed(2)} GB`;
            sdBar.value = used / total * 100;
        } catch {}
    }

    fetchSD(); setInterval(fetchSD, 30000);

    const mgr = document.getElementById('file-manager');
    dirs.forEach(d => createSection(d));

    function createSection(d) {
        const section = document.createElement('div');
        section.className = 'section';
        const header = document.createElement('div');
        header.className = 'section-header';
        header.innerHTML = `<span class="toggle-icon">▶</span><strong>${d.name}</strong>`;

        const btn = document.createElement('button');
        btn.className = 'upload-btn';
        btn.textContent = d.name === 'Shows' ? '+ New Show' : 'Upload';
        const inp = document.createElement('input');
        inp.type = 'file'; inp.multiple = true; inp.className = 'upload-input';
        header.append(btn, inp);

        const content = document.createElement('div');
        content.style.display = 'none';

        header.addEventListener('click', e => {
            if (e.target === btn) return;
            const open = content.style.display === 'block';
            content.style.display = open ? 'none' : 'block';
            header.querySelector('.toggle-icon').textContent = open ? '▶' : '▼';
            if (!content.dataset.loaded) {
                d.name === 'Shows' ? loadShows(content) : loadFiles(d, content);
                content.dataset.loaded = '1';
            }
        });

        btn.addEventListener('click', async e => {
            e.stopPropagation();
            if (d.name === 'Shows') {
                const nm = prompt('New show folder:');
                if (!nm) return;
                const fd = new FormData(); fd.append('dirname', nm);
                if ((await fetch('/mkdir', {method:'POST', body:fd})).ok) location.reload();
            } else {
                inp.click();
            }
        });

        inp.addEventListener('change', async () => {
            for (const file of inp.files) {
                showGlobal('Uploading...');
                gfile.textContent = file.name;
                const xhr = new XMLHttpRequest();
                xhr.open('POST', d.upload);
                xhr.upload.onprogress = e => updateGlobal(e.loaded/e.total*100);
                xhr.onload = () => location.reload();
                const fd = new FormData(); fd.append('file', file);
                xhr.send(fd);
            }
            inp.value = '';
        });

        section.append(header, content);
        mgr.append(section);
    }

    async function loadFiles(d, container) {
        container.innerHTML = '';
        const res = await fetch(`/listfiles?dir=${encodeURIComponent(d.path)}`);
        const files = await res.json();
        files.forEach(f => {
            if (!f.isDir) container.append(makeFileEntry(d.path, f));
        });
    }

    async function loadShows(container) {
        container.innerHTML = '';
        const res = await fetch('/listfiles?dir=/Shows');
        const folders = (await res.json()).filter(f => f.isDir);
        for (const folder of folders) {
            const sec = document.createElement('div');
            sec.className = 'subsection';
            const header = document.createElement('div');
            header.className = 'subsection-header';
            header.innerHTML = `<span class="toggle-icon">▶</span><strong>${folder.name}</strong>`;

            const btn = document.createElement('button');
            btn.className = 'upload-btn'; btn.textContent = 'Upload';
            const inp = document.createElement('input');
            inp.type = 'file'; inp.multiple = true; inp.className = 'upload-input';
            header.append(btn, inp);

            const content = document.createElement('div');
            content.style.display = 'none';

            header.addEventListener('click', e => {
                if (e.target === btn) return;
                const open = content.style.display === 'block';
                content.style.display = open ? 'none' : 'block';
                header.querySelector('.toggle-icon').textContent = open ? '▶' : '▼';
                if (!content.dataset.loaded) {
                    loadSubFiles(`/Shows/${folder.name}`, content);
                    content.dataset.loaded = '1';
                }
            });

            btn.addEventListener('click', e => {
                e.stopPropagation(); inp.click();
            });

            inp.addEventListener('change', async () => {
                for (const file of inp.files) {
                    showGlobal('Uploading...');
                    gfile.textContent = file.name;
                    const xhr = new XMLHttpRequest();
                    xhr.open('POST', `/upload-show?subdir=${encodeURIComponent(folder.name)}`);
                    xhr.upload.onprogress = e => updateGlobal(e.loaded/e.total*100);
                    xhr.onload = () => location.reload();
                    const fd = new FormData(); fd.append('file', file);
                    xhr.send(fd);
                }
                inp.value = '';
            });

            sec.append(header, content);
            container.append(sec);
        }
    }

    async function loadSubFiles(path, container) {
        container.innerHTML = '';
        const res = await fetch(`/listfiles?dir=${encodeURIComponent(path)}`);
        const files = await res.json();
        files.forEach(f => {
            if (!f.isDir) container.append(makeFileEntry(path, f));
        });
    }

    function makeFileEntry(path, file) {
        const div = document.createElement('div');
        div.className = 'file-entry';
        div.innerHTML = `
            <span class="file-name">${file.name}</span>
            <span class="file-size">${(file.size/1024/1024).toFixed(1)} MB</span>
            <button onclick="deleteFile('${path}/${file.name}')">Delete</button>
        `;
        return div;
    }

    // Gestion des LED
    function updateModeUI(mode) {
        Object.values(modeButtons).forEach(btn => btn.classList.remove('active'));
        modeButtons[mode].classList.add('active');
        ledMode = mode;
    }

    function sendModeToServer(mode) {
        fetch('/led-mode', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mode })
        });
    }

    function sendColorToServer(color) {
        fetch('/led-color', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ color })
        });
    }

    colorPicker.addEventListener('input', () => {
        if (ledMode === 'solid') {
            sendColorToServer(colorPicker.value);
        }
    });

    Object.values(modeButtons).forEach(btn => {
        btn.addEventListener('click', () => {
            const mode = btn.id.replace('mode-', '');
            updateModeUI(mode);
            sendModeToServer(mode);
        });
    });

    // Initialisation
    updateModeUI('off');
});

document.addEventListener('DOMContentLoaded', function() {
    // Initialiser l'interface
    initializeUI();
    
    // Mettre à jour le nombre d'utilisateurs connectés
    updateConnectedUsers();
    
    // Mettre à jour l'état de la carte SD
    updateSDStatus();
    
    // Mettre à jour l'état du WiFi
    updateWiFiStatus();
});

function initializeUI() {
    // Afficher le contenu initial
    fetchMediaContent();
}

function fetchMediaContent() {
    fetch('/media')
        .then(response => response.json())
        .then(data => {
            displayMedia(data);
        })
        .catch(error => {
            console.error('Erreur lors du chargement des médias:', error);
            showError('Erreur de chargement des médias');
        });
}

function updateConnectedUsers() {
    fetch('/status/clients')
        .then(response => response.json())
        .then(data => {
            document.getElementById('connected-users').textContent = 
                `${data.count} utilisateurs connectés`;
        })
        .catch(error => {
            console.error('Erreur lors de la récupération des utilisateurs:', error);
        });
}

function updateSDStatus() {
    fetch('/status/sd')
        .then(response => response.json())
        .then(data => {
            const statusElement = document.getElementById('sd-status');
            statusElement.textContent = `Carte SD: ${data.status}`;
            statusElement.className = data.status === 'OK' ? 'sd-ok' : 'sd-error';
        })
        .catch(error => {
            console.error('Erreur lors de la récupération du statut SD:', error);
        });
}

function updateWiFiStatus() {
    fetch('/status/wifi')
        .then(response => response.json())
        .then(data => {
            const statusElement = document.getElementById('wifi-status');
            statusElement.textContent = `WiFi: ${data.status}`;
            statusElement.className = data.status === 'OK' ? 'wifi-ok' : 'wifi-error';
        })
        .catch(error => {
            console.error('Erreur lors de la récupération du statut WiFi:', error);
        });
}

function displayMedia(mediaData) {
    const contentDiv = document.getElementById('content');
    contentDiv.innerHTML = '';

    // Créer la structure de l'interface
    const mediaContainer = document.createElement('div');
    mediaContainer.className = 'media-container';

    // Ajouter les sections pour chaque type de média
    Object.entries(mediaData).forEach(([type, items]) => {
        const section = document.createElement('section');
        section.className = `media-section ${type}`;

        const header = document.createElement('h2');
        header.textContent = type.charAt(0).toUpperCase() + type.slice(1);
        section.appendChild(header);

        const list = document.createElement('div');
        list.className = 'media-list';

        items.forEach(item => {
            const itemElement = document.createElement('div');
            itemElement.className = 'media-item';
            itemElement.innerHTML = `
                <img src="${item.cover}" alt="${item.name}">
                <h3>${item.name}</h3>
                <button onclick="playMedia('${item.file}')">Jouer</button>
            `;
            list.appendChild(itemElement);
        });

        section.appendChild(list);
        mediaContainer.appendChild(section);
    });

    contentDiv.appendChild(mediaContainer);
}

function playMedia(file) {
    window.location.href = `/media?file=${encodeURIComponent(file)}`;
}

function showError(message) {
    const errorDiv = document.createElement('div');
    errorDiv.className = 'error-message';
    errorDiv.textContent = message;
    document.getElementById('content').appendChild(errorDiv);
    setTimeout(() => errorDiv.remove(), 5000);
}
