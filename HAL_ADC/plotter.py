"""
Serial Data Logger with Real-Time Plot and Edge Trigger
- Reads 16-bit big-endian data from serial port (only lower 12 bits used)
- Sample interval: 20 µs
- COM port selectable from list of available ports on the PC
- Settings: 921600 baud, 8 data bits, no parity, 1 stop bit
- Oscilloscope-style edge trigger (Rising / Falling) with settable level
- User-selectable time window
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading
import queue
import struct
import time
import collections

import serial
import serial.tools.list_ports

import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
import matplotlib.lines as mlines


# ── Fixed hardware constants ────────────────────────────────────────────────
BAUD_RATE           = 921600
DATA_BITS           = serial.EIGHTBITS
PARITY              = serial.PARITY_NONE
STOP_BITS           = serial.STOPBITS_ONE
BYTES_PER_SAMPLE    = 2        # 16-bit word
MASK_12BIT          = 0x0FFF   # keep lower 12 bits
SAMPLE_INTERVAL_US  = 50       # µs between samples

# ── Defaults ────────────────────────────────────────────────────────────────
DEFAULT_WINDOW_MS   = 2.0      # time window shown (ms)
DEFAULT_TRIG_LEVEL  = 2048     # trigger threshold (0–4095)
PRE_TRIGGER_FRAC    = 0.15     # fraction of window shown before trigger point

# Trigger state machine
ARMED       = "armed"
CAPTURING   = "capturing"
HOLDING     = "holding"        # frame displayed, waiting before re-arm


def ms_to_samples(ms: float) -> int:
    """Convert a time window in milliseconds to a sample count."""
    return max(2, int(ms * 1000 / SAMPLE_INTERVAL_US))


class SerialLoggerApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Serial Data Logger  –  20 µs / sample")
        self.root.resizable(True, True)

        # Serial
        self.serial_port: serial.Serial | None = None
        self.read_thread: threading.Thread | None = None
        self.running = False

        # Raw-sample queue (filled by reader thread)
        self.data_queue: queue.Queue[int] = queue.Queue(maxsize=200_000)

        # Display buffer (set once trigger fires or in free-run)
        self._window_samples = ms_to_samples(DEFAULT_WINDOW_MS)
        self._pre_samples     = max(1, int(self._window_samples * PRE_TRIGGER_FRAC))
        self._post_samples    = self._window_samples - self._pre_samples

        # Free-run rolling buffer
        self._freerun_buf = collections.deque(
            [0] * self._window_samples, maxlen=self._window_samples
        )

        # Trigger engine state
        self._trig_state    = ARMED
        self._pre_buf       = collections.deque(maxlen=self._pre_samples)
        self._post_buf: list[int] = []
        self._prev_sample: int | None = None
        self._capture_ready: list[int] | None = None  # thread-safe hand-off
        self._capture_lock  = threading.Lock()

        # Display snapshot (what is currently shown)
        self._display_y: list[int] = [0] * self._window_samples
        self._display_x: list[float] = self._make_x_axis(self._window_samples)

        self._build_controls()
        self._build_plot()
        self._start_animation()

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ── Helpers ────────────────────────────────────────────────────────────

    def _make_x_axis(self, n_samples: int) -> list[float]:
        """Return time axis in µs, with trigger point at PRE_TRIGGER_FRAC."""
        pre = max(1, int(n_samples * PRE_TRIGGER_FRAC))
        return [(i - pre) * SAMPLE_INTERVAL_US for i in range(n_samples)]

    def _recalc_window(self):
        """Call whenever window size or pre-trigger fraction changes."""
        try:
            ms = float(self.window_ms_var.get())
        except ValueError:
            return
        ms = max(0.04, min(ms, 1000.0))
        self._window_samples = ms_to_samples(ms)
        self._pre_samples    = max(1, int(self._window_samples * PRE_TRIGGER_FRAC))
        self._post_samples   = self._window_samples - self._pre_samples
        self._freerun_buf    = collections.deque(
            [0] * self._window_samples, maxlen=self._window_samples
        )
        self._pre_buf        = collections.deque(maxlen=self._pre_samples)
        self._display_x      = self._make_x_axis(self._window_samples)
        self._display_y      = [0] * self._window_samples
        self._trig_state     = ARMED
        self._post_buf       = []
        self._prev_sample    = None
        with self._capture_lock:
            self._capture_ready = None
        # Resize plot axes
        win_us = self._window_samples * SAMPLE_INTERVAL_US
        self.ax.set_xlim(self._display_x[0], self._display_x[-1])
        pre_us = self._pre_samples * SAMPLE_INTERVAL_US
        self._update_x_label(win_us)
        self._trig_line.set_xdata([self._display_x[0], self._display_x[-1]])

    def _update_x_label(self, win_us: float):
        if win_us >= 1000:
            self.ax.set_xlabel(
                f"Time relative to trigger (µs)   window={win_us/1000:.2f} ms",
                color="#cdd6f4"
            )
        else:
            self.ax.set_xlabel(
                f"Time relative to trigger (µs)   window={win_us:.0f} µs",
                color="#cdd6f4"
            )

    # ── UI construction ────────────────────────────────────────────────────

    def _build_controls(self):
        # ── Connection row ──────────────────────────────────────────────
        conn_frame = ttk.LabelFrame(self.root, text="Connection", padding=8)
        conn_frame.pack(side=tk.TOP, fill=tk.X, padx=10, pady=(10, 0))

        ttk.Label(conn_frame, text="COM Port:").grid(row=0, column=0, sticky=tk.W, padx=(0, 4))

        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn_frame, textvariable=self.port_var, width=16, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky=tk.W, padx=(0, 4))

        self.refresh_btn = ttk.Button(conn_frame, text="Refresh", command=self._refresh_ports)
        self.refresh_btn.grid(row=0, column=2, padx=(0, 8))

        self.connect_btn = ttk.Button(conn_frame, text="Connect", command=self._toggle_connection)
        self.connect_btn.grid(row=0, column=3, padx=(0, 12))

        ttk.Label(
            conn_frame,
            text=f"{BAUD_RATE} baud  |  8-N-1  |  {SAMPLE_INTERVAL_US} µs/sample",
            foreground="gray"
        ).grid(row=0, column=4, sticky=tk.W)

        # ── Trigger row ─────────────────────────────────────────────────
        trig_frame = ttk.LabelFrame(self.root, text="Trigger", padding=8)
        trig_frame.pack(side=tk.TOP, fill=tk.X, padx=10, pady=(4, 0))

        # Enable checkbox
        self.trig_enabled_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            trig_frame, text="Enable Trigger",
            variable=self.trig_enabled_var,
            command=self._on_trig_changed
        ).grid(row=0, column=0, sticky=tk.W, padx=(0, 14))

        # Edge type
        ttk.Label(trig_frame, text="Edge:").grid(row=0, column=1, sticky=tk.W, padx=(0, 4))
        self.edge_var = tk.StringVar(value="Rising")
        ttk.Combobox(
            trig_frame, textvariable=self.edge_var,
            values=["Rising", "Falling"], width=8, state="readonly"
        ).grid(row=0, column=2, sticky=tk.W, padx=(0, 14))

        # Trigger level
        ttk.Label(trig_frame, text="Level (0–4095):").grid(row=0, column=3, sticky=tk.W, padx=(0, 4))
        self.trig_level_var = tk.IntVar(value=DEFAULT_TRIG_LEVEL)
        trig_spin = ttk.Spinbox(
            trig_frame, from_=0, to=MASK_12BIT, increment=1,
            textvariable=self.trig_level_var, width=6,
            command=self._on_trig_changed
        )
        trig_spin.grid(row=0, column=4, sticky=tk.W, padx=(0, 4))
        trig_spin.bind("<Return>", lambda _: self._on_trig_changed())
        trig_spin.bind("<FocusOut>", lambda _: self._on_trig_changed())

        # Slider for trigger level
        self.trig_slider = ttk.Scale(
            trig_frame, from_=0, to=MASK_12BIT,
            orient=tk.HORIZONTAL, length=160,
            command=self._on_slider_moved
        )
        self.trig_slider.set(DEFAULT_TRIG_LEVEL)
        self.trig_slider.grid(row=0, column=5, sticky=tk.W, padx=(0, 14))

        # Time window
        ttk.Label(trig_frame, text="Window (ms):").grid(row=0, column=6, sticky=tk.W, padx=(0, 4))
        self.window_ms_var = tk.StringVar(value=str(DEFAULT_WINDOW_MS))
        win_spin = ttk.Spinbox(
            trig_frame, from_=0.04, to=1000.0, increment=0.5,
            textvariable=self.window_ms_var, width=7,
            command=self._apply_window
        )
        win_spin.grid(row=0, column=7, sticky=tk.W, padx=(0, 4))
        win_spin.bind("<Return>", lambda _: self._apply_window())
        win_spin.bind("<FocusOut>", lambda _: self._apply_window())

        # Single-shot / Auto
        self.trig_mode_var = tk.StringVar(value="Auto")
        ttk.Radiobutton(trig_frame, text="Auto",   variable=self.trig_mode_var, value="Auto").grid(
            row=0, column=8, padx=(10, 4))
        ttk.Radiobutton(trig_frame, text="Single", variable=self.trig_mode_var, value="Single").grid(
            row=0, column=9, padx=(0, 4))
        self.arm_btn = ttk.Button(trig_frame, text="Arm", command=self._arm_trigger, state="disabled")
        self.arm_btn.grid(row=0, column=10, padx=(4, 0))

        # ── Statistics row ──────────────────────────────────────────────
        stats_frame = ttk.LabelFrame(self.root, text="Statistics", padding=6)
        stats_frame.pack(side=tk.TOP, fill=tk.X, padx=10, pady=(4, 0))

        self.value_var = tk.StringVar(value="Current: ---")
        self.min_var   = tk.StringVar(value="Min: ---")
        self.max_var   = tk.StringVar(value="Max: ---")
        self.avg_var   = tk.StringVar(value="Avg: ---")
        self.trig_var  = tk.StringVar(value="Trigger: ---")

        for col, var in enumerate([
            self.value_var, self.min_var, self.max_var,
            self.avg_var, self.trig_var
        ]):
            ttk.Label(stats_frame, textvariable=var, width=20).grid(row=0, column=col, padx=8)

        # Status bar
        self.status_var = tk.StringVar(value="Disconnected")
        ttk.Label(self.root, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            side=tk.BOTTOM, fill=tk.X
        )

        self._refresh_ports()

    def _build_plot(self):
        plot_frame = ttk.LabelFrame(self.root, text="Signal", padding=6)
        plot_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=10, pady=6)

        self.fig = Figure(figsize=(11, 5), dpi=100)
        self.ax  = self.fig.add_subplot(111)
        self.ax.set_facecolor("#1e1e2e")
        self.fig.patch.set_facecolor("#2a2a3e")

        x = self._display_x
        self.line, = self.ax.plot(x, self._display_y, color="#50fa7b", linewidth=1.0)

        # Trigger level horizontal line
        self._trig_line = self.ax.axhline(
            y=DEFAULT_TRIG_LEVEL, color="#ff5555",
            linewidth=1.0, linestyle="--", label="Trigger level"
        )
        # Trigger point vertical line (x=0)
        self._trig_vline = self.ax.axvline(
            x=0, color="#ffb86c", linewidth=0.8,
            linestyle=":", label="Trigger point"
        )

        win_us = self._window_samples * SAMPLE_INTERVAL_US
        self.ax.set_xlim(x[0], x[-1])
        self.ax.set_ylim(0, MASK_12BIT)
        self._update_x_label(win_us)
        self.ax.set_ylabel("ADC Value (12-bit)", color="#cdd6f4")
        self.ax.set_title("Real-Time Serial Signal", color="#cdd6f4")
        self.ax.tick_params(colors="#cdd6f4")
        for spine in self.ax.spines.values():
            spine.set_edgecolor("#45475a")
        self.ax.grid(True, color="#313244", linestyle="--", linewidth=0.5)
        self.ax.legend(
            handles=[
                mlines.Line2D([], [], color="#ff5555", linestyle="--", label="Trigger level"),
                mlines.Line2D([], [], color="#ffb86c", linestyle=":",  label="Trigger point"),
            ],
            loc="upper right", fontsize=8,
            facecolor="#313244", edgecolor="#45475a", labelcolor="#cdd6f4"
        )

        self.canvas = FigureCanvasTkAgg(self.fig, master=plot_frame)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    # ── Animation ──────────────────────────────────────────────────────────

    def _start_animation(self):
        self.ani = animation.FuncAnimation(
            self.fig, self._update_plot,
            interval=80, blit=False, cache_frame_data=False,
        )

    def _update_plot(self, _frame):
        # Drain the queue and feed through trigger / free-run engine
        samples: list[int] = []
        try:
            while True:
                samples.append(self.data_queue.get_nowait())
        except queue.Empty:
            pass

        triggered = False

        if self.trig_enabled_var.get():
            # ── Triggered mode ──────────────────────────────────────────
            for s in samples:
                if self._trig_state == ARMED:
                    self._pre_buf.append(s)
                    if self._is_trigger(s):
                        self._trig_state = CAPTURING
                        self._post_buf   = [s]
                elif self._trig_state == CAPTURING:
                    self._post_buf.append(s)
                    if len(self._post_buf) >= self._post_samples:
                        # Build full frame: pre + post
                        frame = list(self._pre_buf) + self._post_buf
                        # Pad or trim to exact window size
                        if len(frame) < self._window_samples:
                            frame = [0] * (self._window_samples - len(frame)) + frame
                        else:
                            frame = frame[-self._window_samples:]
                        with self._capture_lock:
                            self._capture_ready = frame
                        self._trig_state = HOLDING
                        triggered = True
                # HOLDING: keep accumulating into pre_buf (for next arm) but don't trigger
                elif self._trig_state == HOLDING:
                    self._pre_buf.append(s)

            # Pick up any newly captured frame
            with self._capture_lock:
                ready = self._capture_ready
                self._capture_ready = None

            if ready is not None:
                self._display_y = ready
                trig_level = self.trig_level_var.get()
                vals = ready
                self.value_var.set(f"Current: {vals[-1]}")
                self.min_var.set(f"Min: {min(vals)}")
                self.max_var.set(f"Max: {max(vals)}")
                self.avg_var.set(f"Avg: {sum(vals)/len(vals):.1f}")
                self.trig_var.set(f"Trigger: {trig_level}  ({self.edge_var.get()})")

                if self.trig_mode_var.get() == "Auto":
                    # Re-arm automatically after displaying
                    self.root.after(50, self._arm_trigger)
                else:
                    # Single mode: stay holding, enable Arm button
                    self.arm_btn.config(state="normal")
                    self.status_var.set("Single capture done — press Arm to re-arm")
        else:
            # ── Free-run mode ───────────────────────────────────────────
            for s in samples:
                self._freerun_buf.append(s)
            if samples:
                vals = list(self._freerun_buf)
                self._display_y = vals
                self.value_var.set(f"Current: {vals[-1]}")
                self.min_var.set(f"Min: {min(vals)}")
                self.max_var.set(f"Max: {max(vals)}")
                self.avg_var.set(f"Avg: {sum(vals)/len(vals):.1f}")
                self.trig_var.set("Trigger: disabled")

        # Refresh chart
        if len(self._display_y) == len(self._display_x):
            self.line.set_data(self._display_x, self._display_y)
        self._trig_line.set_ydata([self.trig_level_var.get(), self.trig_level_var.get()])
        self.canvas.draw_idle()
        return []

    # ── Trigger helpers ────────────────────────────────────────────────────

    def _is_trigger(self, current: int) -> bool:
        prev = self._prev_sample
        self._prev_sample = current
        if prev is None:
            return False
        level = self.trig_level_var.get()
        if self.edge_var.get() == "Rising":
            return prev < level <= current
        else:  # Falling
            return prev > level >= current

    def _on_trig_changed(self):
        """Called when trigger level or edge type changes — re-arm immediately."""
        self._trig_line.set_ydata([self.trig_level_var.get(), self.trig_level_var.get()])
        if self._trig_state != ARMED:
            self._arm_trigger()

    def _on_slider_moved(self, val):
        self.trig_level_var.set(int(float(val)))
        self._on_trig_changed()

    def _arm_trigger(self):
        self._trig_state  = ARMED
        self._post_buf    = []
        self._prev_sample = None
        self.arm_btn.config(state="disabled")
        if self.running:
            self.status_var.set(
                f"Armed — waiting for {self.edge_var.get()} edge at level "
                f"{self.trig_level_var.get()}"
            )

    def _apply_window(self):
        self._recalc_window()

    # ── Serial port management ─────────────────────────────────────────────

    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        ports.sort()
        self.port_combo["values"] = ports
        self.port_combo.set(ports[0] if ports else "")
        self.status_var.set(f"Found {len(ports)} port(s)")

    def _toggle_connection(self):
        if self.running:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.port_var.get()
        if not port:
            messagebox.showerror("Error", "Please select a COM port first.")
            return
        try:
            self.serial_port = serial.Serial(
                port=port, baudrate=BAUD_RATE,
                bytesize=DATA_BITS, parity=PARITY,
                stopbits=STOP_BITS, timeout=0.1,
            )
        except serial.SerialException as exc:
            messagebox.showerror("Connection Error", str(exc))
            return

        self.running = True
        self.connect_btn.config(text="Disconnect")
        self.port_combo.config(state="disabled")
        self.refresh_btn.config(state="disabled")
        self.status_var.set(f"Connected to {port} at {BAUD_RATE} baud")
        self._arm_trigger()

        self.read_thread = threading.Thread(target=self._read_loop, daemon=True)
        self.read_thread.start()

    def _disconnect(self):
        self.running = False
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        self.serial_port = None
        self.connect_btn.config(text="Connect")
        self.port_combo.config(state="readonly")
        self.refresh_btn.config(state="normal")
        self.arm_btn.config(state="disabled")
        self.status_var.set("Disconnected")

    def _read_loop(self):
        """
        Background thread: burst-read from serial, parse 2-byte big-endian
        words, mask to 12 bits, push to queue.
        """
        buf = b""
        while self.running:
            try:
                if not self.serial_port or not self.serial_port.is_open:
                    break
                waiting = self.serial_port.in_waiting
                if waiting >= BYTES_PER_SAMPLE:
                    chunk = self.serial_port.read(waiting - (waiting % BYTES_PER_SAMPLE))
                    buf += chunk
                    while len(buf) >= BYTES_PER_SAMPLE:
                        word  = struct.unpack_from("<H", buf, 0)[0]
                        value = word & MASK_12BIT
                        try:
                            self.data_queue.put_nowait(value)
                        except queue.Full:
                            pass  # drop oldest is not possible with Queue; just skip
                        buf = buf[BYTES_PER_SAMPLE:]
                else:
                    time.sleep(0.001)   # 1 ms wait — at 20 µs/sample we won't lose much
            except serial.SerialException:
                break
            except Exception:
                break

        self.root.after(0, self._on_read_stopped)

    def _on_read_stopped(self):
        if self.running:
            self._disconnect()
            messagebox.showwarning("Warning", "Serial connection was lost.")

    # ── Window close ───────────────────────────────────────────────────────

    def _on_close(self):
        self._disconnect()
        plt.close("all")
        self.root.destroy()


def main():
    root = tk.Tk()
    root.geometry("1200x720")
    app = SerialLoggerApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()