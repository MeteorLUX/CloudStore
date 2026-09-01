const state = {
  connected: false,
  loggedIn: false,
  currentPath: "/",
  polling: null,
};

const $ = (id) => document.getElementById(id);

function toast(msg, isError = false) {
  const el = $("toast");
  el.textContent = msg;
  el.style.borderColor = isError ? "#7f1d1d" : "#2d3b52";
  el.classList.add("show");
  setTimeout(() => el.classList.remove("show"), 2800);
}

async function api(path, options = {}) {
  const res = await fetch(path, {
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
    ...options,
  });
  const data = await res.json().catch(() => ({}));
  if (!data.ok) {
    throw new Error(data.error || `请求失败 (${res.status})`);
  }
  return data.data;
}

function setLoggedInUI(loggedIn, username = "") {
  state.loggedIn = loggedIn;
  $("loginPanel").classList.toggle("hidden", loggedIn);
  $("userPanel").classList.toggle("hidden", !loggedIn);
  $("btnMkdir").disabled = !loggedIn;
  $("fileInput").disabled = !loggedIn;
  $("btnRefresh").disabled = !loggedIn;
  if (loggedIn) {
    $("userName").textContent = username;
    $("avatar").textContent = username.charAt(0).toUpperCase();
    $("userHost").textContent = `${$("host").value}:${$("port").value}`;
  }
}

function joinPath(base, name) {
  if (base === "/") return `/${name}`;
  return `${base.replace(/\/$/, "")}/${name}`;
}

function renderBreadcrumb() {
  const parts = state.currentPath === "/" ? [] : state.currentPath.split("/").filter(Boolean);
  const el = $("breadcrumb");
  el.innerHTML = "";
  const root = document.createElement("span");
  root.className = `crumb ${parts.length === 0 ? "active" : ""}`;
  root.textContent = "/";
  root.onclick = () => {
    if (parts.length) {
      state.currentPath = "/";
      loadFiles();
    }
  };
  el.appendChild(root);
  let acc = "";
  parts.forEach((p, i) => {
    const sep = document.createElement("span");
    sep.textContent = " / ";
    sep.style.color = "#8fa3bf";
    el.appendChild(sep);
    acc += `/${p}`;
    const crumb = document.createElement("span");
    crumb.className = `crumb ${i === parts.length - 1 ? "active" : ""}`;
    crumb.textContent = p;
    const path = acc;
    crumb.onclick = () => {
      if (i < parts.length - 1) {
        state.currentPath = path;
        loadFiles();
      }
    };
    el.appendChild(crumb);
  });
}

function formatSize(n) {
  if (!n) return "-";
  const units = ["B", "KB", "MB", "GB"];
  let v = Number(n);
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(i ? 1 : 0)} ${units[i]}`;
}

async function loadFiles() {
  if (!state.loggedIn) return;
  renderBreadcrumb();
  const list = $("fileList");
  list.innerHTML = `<div class="empty">加载中...</div>`;
  try {
    const data = await api(`/api/ls?path=${encodeURIComponent(state.currentPath)}`);
    const entries = data.entries || [];
    if (!entries.length) {
      list.innerHTML = `<div class="empty">此目录为空</div>`;
      return;
    }
    const rows = entries
      .map((e) => {
        const name = e.name || e.path?.split("/").pop() || "?";
        const type = e.type || "file";
        const fullPath = e.path?.startsWith("/") ? e.path : joinPath(state.currentPath, name);
        const isDir = type === "dir";
        return `
          <tr>
            <td>
              <div class="name-cell" data-type="${type}" data-path="${fullPath}" data-name="${name}">
                <div class="${isDir ? "icon-dir" : "icon-file"}">${isDir ? "📁" : "📄"}</div>
                <span>${name}</span>
              </div>
            </td>
            <td>${type}</td>
            <td>${formatSize(e.size)}</td>
            <td>${e.status || "-"}</td>
            <td class="actions">
              ${isDir ? "" : `<button data-dl="${fullPath}">下载</button>`}
              <button data-rm="${fullPath}" class="danger">删除</button>
            </td>
          </tr>`;
      })
      .join("");
    list.innerHTML = `
      <table>
        <thead><tr><th>名称</th><th>类型</th><th>大小</th><th>状态</th><th>操作</th></tr></thead>
        <tbody>${rows}</tbody>
      </table>`;

    list.querySelectorAll(".name-cell").forEach((cell) => {
      cell.onclick = () => {
        if (cell.dataset.type === "dir") {
          state.currentPath = cell.dataset.path;
          loadFiles();
        }
      };
    });
    list.querySelectorAll("[data-dl]").forEach((btn) => {
      btn.onclick = (ev) => {
        ev.stopPropagation();
        window.open(`/api/download?path=${encodeURIComponent(btn.dataset.dl)}`, "_blank");
      };
    });
    list.querySelectorAll("[data-rm]").forEach((btn) => {
      btn.onclick = async (ev) => {
        ev.stopPropagation();
        if (!confirm(`确定删除 ${btn.dataset.rm} ？`)) return;
        await api("/api/rm", { method: "POST", body: JSON.stringify({ path: btn.dataset.rm }) });
        toast("已删除");
        loadFiles();
      };
    });
  } catch (err) {
    list.innerHTML = `<div class="empty">${err.message}</div>`;
    toast(err.message, true);
  }
}

async function pollProgress() {
  if (!state.loggedIn) return;
  try {
    const st = await api("/api/status");
    const panel = $("progressPanel");
    if (st.progress_phase && st.progress_phase !== "done" && st.progress_phase !== "instant") {
      panel.classList.remove("hidden");
      const total = Number(st.progress_total || 0);
      const current = Number(st.progress_current || 0);
      const pct = total ? Math.min(100, Math.round((current / total) * 100)) : 0;
      $("progressLabel").textContent = `${st.progress_phase} ${pct}%`;
      $("progressFill").style.width = `${pct}%`;
    } else {
      panel.classList.add("hidden");
      $("progressFill").style.width = "0%";
    }
  } catch (_) {}
}

$("btnConnect").onclick = async () => {
  try {
    await api("/api/connect", {
      method: "POST",
      body: JSON.stringify({ host: $("host").value, port: Number($("port").value) }),
    });
    state.connected = true;
    toast("已连接服务端");
  } catch (err) {
    toast(err.message, true);
  }
};

$("btnLogin").onclick = async () => {
  try {
    if (!state.connected) await $("btnConnect").onclick();
    const data = await api("/api/login", {
      method: "POST",
      body: JSON.stringify({ username: $("username").value, password: $("password").value }),
    });
    setLoggedInUI(true, data.username || $("username").value);
    toast("登录成功");
    state.currentPath = "/";
    loadFiles();
    if (!state.polling) state.polling = setInterval(pollProgress, 500);
  } catch (err) {
    toast(err.message, true);
  }
};

$("btnRegister").onclick = async () => {
  try {
    if (!state.connected) await $("btnConnect").onclick();
    await api("/api/register", {
      method: "POST",
      body: JSON.stringify({ username: $("username").value, password: $("password").value }),
    });
    toast("注册成功，请登录");
  } catch (err) {
    toast(err.message, true);
  }
};

$("btnLogout").onclick = async () => {
  await api("/api/logout", { method: "POST", body: "{}" });
  setLoggedInUI(false);
  $("fileList").innerHTML = `<div class="empty">请先连接并登录</div>`;
  toast("已退出");
};

$("btnMkdir").onclick = async () => {
  const name = prompt("文件夹名称");
  if (!name) return;
  const path = joinPath(state.currentPath, name);
  await api("/api/mkdir", { method: "POST", body: JSON.stringify({ path }) });
  toast("文件夹已创建");
  loadFiles();
};

$("btnRefresh").onclick = () => loadFiles();

$("fileInput").onchange = async (ev) => {
  const file = ev.target.files[0];
  if (!file) return;
  const remoteName = prompt("云端路径（文件名）", file.name);
  if (!remoteName) return;
  const path = joinPath(state.currentPath, remoteName);
  const form = new FormData();
  form.append("file", file);
  form.append("path", path);
  form.append("overwrite", "true");
  $("progressPanel").classList.remove("hidden");
  $("progressLabel").textContent = "准备上传...";
  try {
    const res = await fetch("/api/upload", { method: "POST", body: form });
    const data = await res.json();
    if (!data.ok) throw new Error(data.error || "上传失败");
    toast(data.data.mode === "instant" ? "秒传成功" : "上传完成");
    loadFiles();
  } catch (err) {
    toast(err.message, true);
  } finally {
    ev.target.value = "";
    $("progressPanel").classList.add("hidden");
  }
};

(async function init() {
  try {
    const st = await api("/api/status");
    state.connected = st.connected;
    if (st.logged_in) {
      setLoggedInUI(true, st.username);
      loadFiles();
      state.polling = setInterval(pollProgress, 500);
    }
  } catch (_) {}
})();
