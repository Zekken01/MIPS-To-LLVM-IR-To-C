"""Configure and run the Windows native PS2 launcher."""

import os
from pathlib import Path
import subprocess
import tkinter as tk
from tkinter import filedialog, ttk

ROOT = Path(__file__).resolve().parents[1]
LOG_PATH = ROOT / "build" / "ps2-launcher.log"


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

        self.path_row(0, "Extracted disc", self.disc, filedialog.askdirectory)
        self.path_row(1, "Memory card", self.card, filedialog.askopenfilename)
        ttk.Label(self, text="The first connected controller is mapped to PS2 port 1.").grid(
            row=2, column=0, columnspan=3, sticky="w", pady=(4, 10)
        )
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
            self.status.set("The native raylib window is available on Windows.")

    def path_row(self, row, label, variable, picker):
        def browse():
            selected = picker(parent=self)
            if selected:
                variable.set(selected)

        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=(0, 12))
        ttk.Entry(self, textvariable=variable).grid(
            row=row, column=1, sticky="ew", pady=4
        )
        ttk.Button(self, text="Browse", command=browse).grid(
            row=row, column=2, padx=(10, 0)
        )

    def launch(self):
        disc = Path(self.disc.get()).expanduser().resolve()
        card_text = self.card.get().strip()
        card = Path(card_text).expanduser().resolve() if card_text else None
        try:
            if not (disc / "SYSTEM.CNF").is_file():
                self.status.set("Choose the extracted disc folder containing SYSTEM.CNF.")
                return
            if card is not None and (not card.is_file() or card.stat().st_size != 8_650_752):
                self.status.set("Choose an 8,650,752-byte raw PS2 memory card image.")
                return
            env = os.environ.copy()
            env.update(SDBZ_DISC_ROOT=str(disc), EE_WEBGPU_WINDOW="1")
            env.pop("SDBZ_MEMCARD", None)
            if card is not None:
                env["SDBZ_MEMCARD"] = str(card)
            LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
            with LOG_PATH.open("wb") as output:
                self.process = subprocess.Popen(
                    ["cmd.exe", "/d", "/c", "run_ps2_game.bat", "--full", "--neutral"],
                    cwd=ROOT,
                    env=env,
                    stdin=subprocess.DEVNULL,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
            self.log = LOG_PATH.open(encoding="utf-8", errors="replace")
        except OSError as error:
            self.status.set(str(error))
            return

        self.output.configure(state="normal")
        self.output.delete("1.0", tk.END)
        self.output.configure(state="disabled")
        self.play.state(["disabled"])
        self.status.set("Building or running the game")
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
            self.status.set(f"Launcher exited ({result}). Log: {LOG_PATH.name}")


def main():
    window = tk.Tk()
    window.title("Native PS2 Launcher")
    window.columnconfigure(0, weight=1)
    window.rowconfigure(0, weight=1)
    Launcher(window)
    window.mainloop()


if __name__ == "__main__":
    main()
