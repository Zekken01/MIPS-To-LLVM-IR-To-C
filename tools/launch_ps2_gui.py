"""Configure and run the Windows native PS2 launcher."""

import os
from pathlib import Path
import subprocess
import sys
import tkinter as tk
from tkinter import filedialog, ttk

ROOT = Path(__file__).resolve().parents[1]
LOG_PATH = ROOT / "build" / "ps2-launcher.log"


SESSION_MODES = {
    "Full replay": "--full",
    "Automatic": None,
    "Quick probe": "--quick",
    "Matched neutral/Cross": "--matched",
    "Recover translation cache": "--recover-cache",
}
PAD_PROFILES = {"Neutral": "--neutral", "Cross replay": "--cross", "Custom": None}
NUMERIC_SETTINGS = {
    "Cross first poll": ("SDBZ_PAD0_CROSS_FIRST_POLL", 1, 0xFFFFFFFF),
    "Cross poll count": ("SDBZ_PAD0_CROSS_POLL_COUNT", 1, 0xFFFFFFFF),
    "Max dispatches": ("MAX_DISPATCHES", 1, None),
    "Timeout (seconds)": ("BOOT_TIMEOUT_SECONDS", 1, None),
    "Expansion passes": ("EXPANSION_PASSES", 1, 64),
    "Functions per expansion": ("EXPANSION_PER_PASS", 1, 128),
    "Compile jobs": ("EMIT_JOBS", 1, None),
    "SIF poll limit": ("SIF_POLL_LIMIT", 1, 0xFFFFFFFF),
    "Loop watchdog resumes": ("LOOP_YIELD_RESUMES", 0, 0xFFFFFFFF),
    "Trace every N dispatches": ("SDBZ_DISPATCH_TRACE_EVERY", 1, 0xFFFFFFFF),
    "Report snapshot interval (ms)": ("SDBZ_REPORT_SNAPSHOT_MS", 0, 60000),
}
COMMAND_TOOLS = ("Boot session", "Game runner (Python)", "Native runner")


def launcher_options(mode, pad, accept_new_ir, window, snapshot, values):
    args = []
    mode_flag = SESSION_MODES[mode]
    if mode_flag:
        args.append(mode_flag)
    if mode == "Recover translation cache" and accept_new_ir:
        args.append("--accept-new-ir")
    env = {}
    if mode in ("Automatic", "Full replay"):
        env["EE_WEBGPU_WINDOW"] = str(int(window))
        env["EE_WEBGPU_SNAPSHOT"] = str(int(snapshot))
    uses_pad_profile = mode not in ("Matched neutral/Cross", "Recover translation cache")
    if uses_pad_profile and PAD_PROFILES[pad]:
        args.append(PAD_PROFILES[pad])

    for label, (key, minimum, maximum) in NUMERIC_SETTINGS.items():
        value = values[label].strip()
        if key.startswith("SDBZ_PAD0_CROSS_") and not (uses_pad_profile and pad == "Custom"):
            continue
        if not value:
            continue
        if not value.isascii() or not value.isdecimal():
            raise ValueError(f"{label} must be a whole number.")
        number = int(value)
        if number < minimum or (maximum is not None and number > maximum):
            limit = f"{minimum}–{maximum}" if maximum is not None else f"{minimum} or more"
            raise ValueError(f"{label} must be {limit}.")
        env[key] = str(number)
    if uses_pad_profile and pad == "Custom":
        if not all(key in env for key in ("SDBZ_PAD0_CROSS_FIRST_POLL", "SDBZ_PAD0_CROSS_POLL_COUNT")):
            raise ValueError("Set both values for the custom Cross pulse.")
    return args, env


def direct_command(tool, executable, arguments, help_only=False):
    if tool == "Native runner":
        path = Path(executable).expanduser().resolve()
        if not path.is_file():
            raise ValueError("Choose an existing native runner executable.")
        command = [str(path)]
    else:
        script = "run_boot_session.py" if tool == "Boot session" else "run_game.py"
        command = [sys.executable, str(ROOT / "tools" / script)]
    if help_only:
        return command + ["--help"]
    # One argument per line keeps Windows paths and whitespace literal.
    return command + [line for line in arguments.splitlines() if line.strip()]


class Launcher(ttk.Frame):
    def __init__(self, window):
        super().__init__(window, padding=18)
        self.grid(sticky="nsew")
        self.columnconfigure(1, weight=1)
        self.rowconfigure(4, weight=1)
        self.disc = tk.StringVar(value=str(ROOT / "PS2_GAME"))
        self.card = tk.StringVar()
        self.status = tk.StringVar()
        self.process = None
        self.log = None

        self.mode = tk.StringVar(value="Full replay")
        self.pad = tk.StringVar(value="Neutral")
        self.accept_ir = tk.BooleanVar()
        self.window = tk.BooleanVar(value=True)
        self.snapshot = tk.BooleanVar(value=True)
        self.numeric = {label: tk.StringVar() for label in NUMERIC_SETTINGS}
        self.tool = tk.StringVar(value=COMMAND_TOOLS[0])
        self.executable = tk.StringVar()
        self.tabs = ttk.Notebook(self)
        self.tabs.grid(row=0, column=0, columnspan=3, sticky="ew", pady=(0, 10))
        self.setup = ttk.Frame(self.tabs, padding=10)
        self.limits = ttk.Frame(self.tabs, padding=10)
        self.direct = ttk.Frame(self.tabs, padding=10)
        for tab, title in (
            (self.setup, "Game"), (self.limits, "Limits and diagnostics"),
            (self.direct, "Direct command"),
        ):
            tab.columnconfigure(1, weight=1)
            self.tabs.add(tab, text=title)
        self.path_row(self.setup, 0, "Extracted disc", self.disc, filedialog.askdirectory)
        self.path_row(self.setup, 1, "Memory card", self.card, filedialog.askopenfilename)
        self.choice_row(self.setup, 2, "Session mode", self.mode, SESSION_MODES)
        self.pad_picker = self.choice_row(self.setup, 3, "Controller profile", self.pad, PAD_PROFILES)
        self.window_picker = ttk.Checkbutton(self.setup, text="WebGPU window", variable=self.window)
        self.window_picker.grid(row=4, column=1, sticky="w")
        self.snapshot_picker = ttk.Checkbutton(self.setup, text="WebGPU snapshot", variable=self.snapshot)
        self.snapshot_picker.grid(row=5, column=1, sticky="w")
        self.ir_picker = ttk.Checkbutton(self.setup, text="Accept changed IR hashes", variable=self.accept_ir)
        self.ir_picker.grid(row=6, column=1, sticky="w")
        self.cross_entries = []
        for row, (label, variable) in enumerate(self.numeric.items()):
            ttk.Label(self.limits, text=label).grid(row=row, column=0, sticky="w", padx=(0, 12))
            entry = ttk.Entry(self.limits, textvariable=variable)
            entry.grid(row=row, column=1, sticky="ew", pady=2)
            if row < 2:
                self.cross_entries.append(entry)
        ttk.Label(self.limits, text="Blank fields use the batch launcher defaults.").grid(
            row=len(self.numeric), column=0, columnspan=2, sticky="w", pady=(6, 0)
        )
        self.choice_row(self.direct, 0, "Tool", self.tool, COMMAND_TOOLS)
        self.path_row(self.direct, 1, "Native executable", self.executable, filedialog.askopenfilename)
        ttk.Label(self.direct, text="Arguments (one per line)").grid(row=2, column=0, sticky="nw")
        self.arguments = tk.Text(self.direct, height=7, width=48, wrap="none")
        self.arguments.grid(row=2, column=1, columnspan=2, sticky="ew")
        self.help_button = ttk.Button(self.direct, text="Show --help", command=lambda: self.launch(True))
        self.help_button.grid(row=3, column=1, sticky="w", pady=(6, 0))
        self.mode.trace_add("write", self.update_fields)
        self.pad.trace_add("write", self.update_fields)
        self.update_fields()
        self.play = ttk.Button(self, text="Launch", command=self.launch)
        self.play.grid(row=3, column=0, sticky="w", pady=(0, 10))
        ttk.Label(self, textvariable=self.status).grid(
            row=3, column=1, columnspan=2, sticky="w"
        )
        self.output = tk.Text(self, height=16, width=80, wrap="word", state="disabled")
        self.output.grid(row=4, column=0, columnspan=2, sticky="nsew")
        scrollbar = ttk.Scrollbar(self, command=self.output.yview)
        scrollbar.grid(row=4, column=2, sticky="ns")
        self.output.configure(yscrollcommand=scrollbar.set)
        if os.name != "nt":
            self.play.state(["disabled"])
            self.help_button.state(["disabled"])
            self.status.set("The native raylib window is available on Windows.")

    def update_fields(self, *_):
        mode = self.mode.get()
        special = mode in ("Matched neutral/Cross", "Recover translation cache")
        self.pad_picker.configure(state="disabled" if special else "readonly")
        graphics_state = ["!disabled"] if mode in ("Automatic", "Full replay") else ["disabled"]
        self.window_picker.state(graphics_state)
        self.snapshot_picker.state(graphics_state)
        self.ir_picker.state(["!disabled"] if mode == "Recover translation cache" else ["disabled"])
        cross_state = ["!disabled"] if not special and self.pad.get() == "Custom" else ["disabled"]
        for entry in self.cross_entries:
            entry.state(cross_state)

    def choice_row(self, parent, row, label, variable, choices):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 12))
        picker = ttk.Combobox(parent, textvariable=variable, values=list(choices), state="readonly")
        picker.grid(row=row, column=1, columnspan=2, sticky="ew", pady=4)
        return picker

    def path_row(self, parent, row, label, variable, picker):
        def browse():
            selected = picker(parent=self)
            if selected:
                variable.set(selected)

        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 12))
        ttk.Entry(parent, textvariable=variable).grid(
            row=row, column=1, sticky="ew", pady=4
        )
        ttk.Button(parent, text="Browse", command=browse).grid(
            row=row, column=2, padx=(10, 0)
        )

    def game_command(self, env):
        disc = Path(self.disc.get()).expanduser().resolve()
        if not (disc / "SYSTEM.CNF").is_file():
            raise ValueError("Choose the extracted disc folder containing SYSTEM.CNF.")
        args, overrides = launcher_options(
            self.mode.get(), self.pad.get(), self.accept_ir.get(), self.window.get(),
            self.snapshot.get(), {label: value.get() for label, value in self.numeric.items()},
        )
        for key, _, _ in NUMERIC_SETTINGS.values():
            env.pop(key, None)
        env.update(overrides, SDBZ_DISC_ROOT=str(disc))
        return ["cmd.exe", "/d", "/c", "run_ps2_game.bat", *args]

    def launch(self, help_only=False):
        if self.process is not None:
            return
        try:
            env = os.environ.copy()
            is_direct = self.tabs.select() == str(self.direct)
            if is_direct:
                command = direct_command(
                    self.tool.get(), self.executable.get(), self.arguments.get("1.0", "end-1c"),
                    help_only,
                )
            else:
                command = self.game_command(env)
            if not help_only:
                card_text = self.card.get().strip()
                env.pop("SDBZ_MEMCARD", None)
                if card_text and (is_direct or self.mode.get() != "Recover translation cache"):
                    card = Path(card_text).expanduser().resolve()
                    if not card.is_file() or card.stat().st_size != 8_650_752:
                        raise ValueError("Choose an 8,650,752-byte raw PS2 memory card image.")
                    env["SDBZ_MEMCARD"] = str(card)
            LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
            with LOG_PATH.open("wb") as output:
                self.process = subprocess.Popen(
                    command,
                    cwd=ROOT,
                    env=env,
                    stdin=subprocess.DEVNULL,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
            self.log = LOG_PATH.open(encoding="utf-8", errors="replace")
        except (OSError, ValueError) as error:
            self.status.set(str(error))
            return

        self.output.configure(state="normal")
        self.output.delete("1.0", tk.END)
        self.output.configure(state="disabled")
        self.play.state(["disabled"])
        self.help_button.state(["disabled"])
        self.status.set("Running command")
        self.read_output()

    def read_output(self):
        chunk = self.log.read(32768)
        if chunk:
            self.output.configure(state="normal")
            self.output.insert(tk.END, chunk)
            lines = int(self.output.index("end-1c").split(".")[0])
            if lines > 2000:
                self.output.delete("1.0", f"{lines - 2000}.0")
            self.output.see(tk.END)
            self.output.configure(state="disabled")

        result = self.process.poll()
        if chunk or result is None:
            self.after(150, self.read_output)
        else:
            self.log.close()
            self.log = None
            self.process = None
            self.play.state(["!disabled"])
            self.help_button.state(["!disabled"])
            self.status.set(f"Launcher exited ({result}). Log: {LOG_PATH.name}")


def main():
    window = tk.Tk()
    window.title("Native PS2 Launcher")
    window.geometry("780x760")
    window.columnconfigure(0, weight=1)
    window.rowconfigure(0, weight=1)
    Launcher(window)
    window.mainloop()


if __name__ == "__main__":
    main()
