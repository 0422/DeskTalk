
import os
import subprocess
import threading
import random
import json
from PIL import Image
import tkinter as tk
import customtkinter as ctk
import webbrowser

from common import *
from connect import *
from audio import *
from gpt import *

blt = BluetoothClient()
ser = SerialClient()
llm = GPT()
listener = Listener(llm)
speaker = Speaker(llm)


class App(ctk.CTk):
    def __init__(self):

        super().__init__()
        title = f"Desk-Emoji {VERSION}"

        # flags
        self.checked = False
        self.api_connected = False
        self.usb_connected = False
        self.blt_connected = False
        self.firmware = ""

        # init window
        self.title(title)
        self.window_width = 700
        self.window_height = 510
        self.geometry(f"{self.window_width}x{self.window_height}")
        self.resizable(False, False)
        self.center_window()

        # set grid layout 1x2
        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(1, weight=1)

        # load images with light and dark mode image
        icon_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), "icons")
        self.logo_image = ctk.CTkImage(Image.open(os.path.join(icon_path, "main_icon.png")), size=(26, 26))
        self.chat_image = ctk.CTkImage(light_image=Image.open(os.path.join(icon_path, "chat_dark.png")),
                                       dark_image=Image.open(os.path.join(icon_path, "chat_light.png")), size=(20, 20))
        self.act_image = ctk.CTkImage(light_image=Image.open(os.path.join(icon_path, "act_dark.png")),
                                      dark_image=Image.open(os.path.join(icon_path, "act_light.png")), size=(20, 20))
        self.usb_icon = ctk.CTkImage(light_image=Image.open(os.path.join(icon_path, "usb_dark.png")),
                                     dark_image=Image.open(os.path.join(icon_path, "usb_light.png")), size=(20, 20))
        self.api_icon = ctk.CTkImage(light_image=Image.open(os.path.join(icon_path, "api_dark.png")),
                                     dark_image=Image.open(os.path.join(icon_path, "api_light.png")), size=(20, 20))
        self.firmware_icon = ctk.CTkImage(light_image=Image.open(os.path.join(icon_path, "firmware_dark.png")),
                                          dark_image=Image.open(os.path.join(icon_path, "firmware_light.png")),
                                          size=(20, 20))
        self.help_icon = ctk.CTkImage(light_image=Image.open(os.path.join(icon_path, "help_dark.png")),
                                      dark_image=Image.open(os.path.join(icon_path, "help_light.png")), size=(20, 20))

        # 新增codex额度查询图标
        self.quota_image = ctk.CTkImage(light_image=Image.open(os.path.join(icon_path, "quota_dark.png")),
                                        dark_image=Image.open(os.path.join(icon_path, "quota_light.png")),
                                        size=(20, 20))

        # create navigation frame
        self.navigation_frame = ctk.CTkFrame(self, corner_radius=0)
        self.navigation_frame.grid(row=0, column=0, sticky="nsew")
        self.navigation_frame.grid_rowconfigure(7, weight=1)

        self.navigation_frame_label = ctk.CTkLabel(self.navigation_frame, text="  Desk-Emoji", image=self.logo_image,
                                                   compound="left", font=ctk.CTkFont(size=15, weight="bold"))
        self.navigation_frame_label.grid(row=0, column=0, padx=20, pady=20)

        self.chat_button = ctk.CTkButton(self.navigation_frame, corner_radius=0, height=40, border_spacing=10,
                                         text="对话",
                                         fg_color="transparent", text_color=("gray10", "gray90"),
                                         hover_color=("gray70", "gray30"),
                                         image=self.chat_image, anchor="w", command=self.chat_button_event)
        self.chat_button.grid(row=1, column=0, sticky="ew")

        self.act_button = ctk.CTkButton(self.navigation_frame, corner_radius=0, height=40, border_spacing=10,
                                        text="动作",
                                        fg_color="transparent", text_color=("gray10", "gray90"),
                                        hover_color=("gray70", "gray30"),
                                        image=self.act_image, anchor="w", command=self.act_button_event)
        self.act_button.grid(row=2, column=0, sticky="ew")

        self.connect_button = ctk.CTkButton(self.navigation_frame, corner_radius=0, height=40, border_spacing=10,
                                            text="串口",
                                            fg_color="transparent", text_color=("gray10", "gray90"),
                                            hover_color=("gray70", "gray30"),
                                            image=self.usb_icon, anchor="w", command=self.connect_button_event)
        self.connect_button.grid(row=3, column=0, sticky="ew")

        self.api_button = ctk.CTkButton(self.navigation_frame, corner_radius=0, height=40, border_spacing=10,
                                        text="API",
                                        fg_color="transparent", text_color=("gray10", "gray90"),
                                        hover_color=("gray70", "gray30"),
                                        image=self.api_icon, anchor="w", command=self.api_button_event)
        self.api_button.grid(row=4, column=0, sticky="ew")

        self.firmware_button = ctk.CTkButton(self.navigation_frame, corner_radius=0, height=40, border_spacing=10,
                                             text="固件",
                                             fg_color="transparent", text_color=("gray10", "gray90"),
                                             hover_color=("gray70", "gray30"),
                                             image=self.firmware_icon, anchor="w", command=self.firmware_button_event)
        self.firmware_button.grid(row=5, column=0, sticky="ew")

        # 新增codex额度查询按钮
        self.quota_button = ctk.CTkButton(self.navigation_frame, corner_radius=0, height=40, border_spacing=10,
                                          text="额度",
                                          fg_color="transparent", text_color=("gray10", "gray90"),
                                          hover_color=("gray70", "gray30"),
                                          image=self.quota_image, anchor="w", command=self.quota_button_event)
        self.quota_button.grid(row=6, column=0, sticky="ew")

        self.help_button = ctk.CTkButton(self.navigation_frame, corner_radius=0, height=40, border_spacing=10,
                                         text="帮助",
                                         fg_color="transparent", text_color=("gray10", "gray90"),
                                         hover_color=("gray70", "gray30"),
                                         image=self.help_icon, anchor="w", command=self.help_button_event)
        self.help_button.grid(row=7, column=0, sticky="ew")

        self.appearance_mode_menu = ctk.CTkOptionMenu(self.navigation_frame, values=["System", "Light", "Dark"],
                                                      command=self.change_appearance_mode_event)
        self.appearance_mode_menu.grid(row=8, column=0, padx=20, pady=20, sticky="s")

        # create chat frame
        self.chat_frame = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.chat_frame.grid_columnconfigure(0, weight=1)
        self.chat_frame.grid_columnconfigure(1, weight=1)
        self.chat_frame.grid_columnconfigure(2, weight=1)

        self.textbox = ctk.CTkTextbox(self.chat_frame, height=300)
        self.textbox.grid(row=0, column=0, columnspan=3, padx=(20, 20), pady=(20, 20), sticky="nsew")

        self.chat_msg = ctk.CTkEntry(self.chat_frame)
        self.chat_msg.grid(row=1, column=0, columnspan=2, padx=20, pady=0, sticky="ew")
        self.chat_msg.bind("<Return>", self.chat_msg_event)

        self.send_button = ctk.CTkButton(self.chat_frame, text="发送", height=40,
                                         command=self.chat_msg_event)
        self.send_button.grid(row=1, column=2, padx=20, pady=20, sticky='e')

        self.speaker_switch = ctk.CTkSwitch(self.chat_frame, text="扬声器")
        self.speaker_switch.grid(row=2, column=0, padx=20, pady=20, sticky="nsew")
        self.speaker_switch.select()

        self.voice_combobox = ctk.CTkComboBox(self.chat_frame,
                                              values=['onyx', 'alloy', 'echo', 'fable', 'nova', 'shimmer'])
        self.voice_combobox.grid(row=2, column=1, padx=20, pady=20, sticky="ew")
        self.voice_combobox.set('onyx')

        self.speech_button = ctk.CTkButton(self.chat_frame, text="语音", height=40,
                                           command=self.speech_button_event)
        self.speech_button.grid(row=2, column=2, padx=20, pady=20, sticky='e')
        self.origin_fg_color = self.speech_button.cget("fg_color")
        self.origin_hover_color = self.speech_button.cget("hover_color")
        self.origin_text_color = self.speech_button.cget("text_color")

        # create act frame
        self.act_frame = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.act_frame.grid_columnconfigure(0, weight=1)
        self.act_frame.grid_columnconfigure(1, weight=1)

        for i, (button_name, button_command) in enumerate(eye_button_list):
            button = ctk.CTkButton(
                self.act_frame,
                text=button_name,
                command=lambda cmd=button_command: self.send_cmd(cmd)
            )
            button.grid(row=i, column=0, padx=10, pady=10, sticky='w')

        button = ctk.CTkButton(self.act_frame, text="测试动画",
                               command=lambda: self.send_cmd(random.choice(animations_list)))
        button.grid(row=len(eye_button_list) + 1, column=0, padx=10, pady=10, sticky='w')

        for i, (button_name, button_command) in enumerate(head_button_list):
            button = ctk.CTkButton(
                self.act_frame,
                text=button_name,
                command=lambda cmd=button_command: self.send_cmd(cmd)
            )
            button.grid(row=i, column=1, padx=10, pady=10, sticky='w')

        # create connect frame
        self.connect_frame = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.connect_frame.grid_columnconfigure(0, weight=1)

        self.connect_tabview = ctk.CTkTabview(self.connect_frame)
        self.connect_tabview.grid(row=0, column=0, padx=20, pady=20, sticky="nsew")
        self.connect_tabview.add("蓝牙")
        self.connect_tabview.tab("蓝牙").grid_columnconfigure(0, weight=1)
        self.connect_tabview.add("USB")
        self.connect_tabview.tab("USB").grid_columnconfigure(0, weight=1)

        self.blt_combobox = ctk.CTkComboBox(self.connect_tabview.tab("蓝牙"), values=[])
        self.blt_combobox.grid(row=0, column=0, columnspan=2, padx=20, pady=20, sticky="nsew")
        self.blt_combobox.set("")

        self.blt_refresh_button = ctk.CTkButton(self.connect_tabview.tab("蓝牙"), text="刷新",
                                                command=self.blt_refresh_button_event)
        self.blt_refresh_button.grid(row=1, column=1, padx=20, pady=10)

        self.blt_connect_button = ctk.CTkButton(self.connect_tabview.tab("蓝牙"), text="连接",
                                                command=self.blt_connect_button_event)
        self.blt_connect_button.grid(row=2, column=1, padx=20, pady=10)

        self.blt_flag_label = ctk.CTkLabel(self.connect_tabview.tab("蓝牙"), text="")
        self.blt_flag_label.grid(row=2, column=0, padx=20, pady=10)

        self.usb_combobox = ctk.CTkComboBox(self.connect_tabview.tab("USB"), values=[])
        self.usb_combobox.grid(row=0, column=0, columnspan=2, padx=20, pady=20, sticky="nsew")
        self.usb_combobox.set("")

        self.usb_refresh_button = ctk.CTkButton(self.connect_tabview.tab("USB"), text="刷新",
                                                command=self.usb_refresh_button_event)
        self.usb_refresh_button.grid(row=1, column=1, padx=20, pady=10)

        self.usb_connect_button = ctk.CTkButton(self.connect_tabview.tab("USB"), text="连接",
                                                command=self.usb_connect_button_event)
        self.usb_connect_button.grid(row=2, column=1, padx=20, pady=10)

        self.usb_flag_label = ctk.CTkLabel(self.connect_tabview.tab("USB"), text="")
        self.usb_flag_label.grid(row=2, column=0, padx=20, pady=10)

        # create api frame
        self.api_frame = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.api_frame.grid_columnconfigure(0, weight=1)

        self.api_tabview = ctk.CTkTabview(self.api_frame)
        self.api_tabview.grid(row=0, column=0, padx=20, pady=20, sticky="nsew")
        self.api_tabview.add("OpenAI")
        self.api_tabview.tab("OpenAI").grid_columnconfigure(0, weight=1)
        self.api_tabview.tab("OpenAI").grid_columnconfigure(1, weight=6)

        self.api_url_label = ctk.CTkLabel(self.api_tabview.tab("OpenAI"), text="API URL: ")
        self.api_url_label.grid(row=0, column=0, padx=20, pady=20, sticky="w")
        self.api_url_entry = ctk.CTkEntry(self.api_tabview.tab("OpenAI"))
        self.api_url_entry.grid(row=0, column=1, padx=20, pady=20, sticky="nsew")

        self.api_key_label = ctk.CTkLabel(self.api_tabview.tab("OpenAI"), text="API Key: ")
        self.api_key_label.grid(row=1, column=0, padx=20, pady=20, sticky="w")
        self.api_key_entry = ctk.CTkEntry(self.api_tabview.tab("OpenAI"))
        self.api_key_entry.grid(row=1, column=1, padx=20, pady=20, sticky="nsew")

        self.save_flag_label = ctk.CTkLabel(self.api_tabview.tab("OpenAI"), text="")
        self.save_flag_label.grid(row=2, column=0, padx=20, pady=20)

        self.api_save_button = ctk.CTkButton(self.api_tabview.tab("OpenAI"), text="连接",
                                             command=self.api_save_button_event)
        self.api_save_button.grid(row=2, column=1, padx=20, pady=10)

        # create firmware frame
        self.firmware_frame = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.firmware_frame.grid_columnconfigure(0, weight=1)
        self.firmware_frame.grid_columnconfigure(0, weight=0)

        self.firmware_entry = ctk.CTkEntry(self.firmware_frame, width=300)
        self.firmware_entry.grid(row=0, column=0, padx=20, pady=20, sticky="w")
        self.firmware_import_button = ctk.CTkButton(self.firmware_frame, text="导入", command=self.import_firmware)
        self.firmware_import_button.grid(row=0, column=1, padx=20, pady=20, sticky="e")

        self.serial_combobox = ctk.CTkComboBox(self.firmware_frame, width=300, values=ser.list_ports())
        self.serial_combobox.grid(row=1, column=0, padx=20, pady=10, sticky="w")
        self.ser_refresh_button = ctk.CTkButton(self.firmware_frame, text="刷新", command=self.ser_refresh_button_event)
        self.ser_refresh_button.grid(row=1, column=1, padx=20, pady=10, sticky="e")

        self.terminal_textbox = ctk.CTkTextbox(self.firmware_frame, width=500, height=300)
        self.terminal_textbox.grid(row=2, column=0, columnspan=2, padx=10, pady=10, sticky="nsew")

        self.open_url_button = ctk.CTkButton(self.firmware_frame, text="固件下载", command=self.open_url)
        self.open_url_button.grid(row=3, column=0, padx=20, pady=10, sticky="w")
        self.burn_button = ctk.CTkButton(self.firmware_frame, text="烧录", command=self.burn_firmware)
        self.burn_button.grid(row=3, column=1, padx=20, pady=10)

        # ========== 创建 quota frame（额度查询页面） ==========
        self.quota_frame = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.quota_frame.grid_columnconfigure(0, weight=1)

        self.quota_query_button = ctk.CTkButton(self.quota_frame, text="📊 查询 Codex 额度",
                                                command=self.query_and_send_quota,
                                                height=50, font=ctk.CTkFont(size=16, weight="bold"))
        self.quota_query_button.grid(row=0, column=0, padx=20, pady=20)

        self.quota_textbox = ctk.CTkTextbox(self.quota_frame, height=300)
        self.quota_textbox.grid(row=1, column=0, padx=20, pady=(0, 20), sticky="nsew")

        self.quota_send_button = ctk.CTkButton(self.quota_frame, text="📤 发送到机器人显示",
                                               command=self.send_quota_to_robot,
                                               height=40)
        self.quota_send_button.grid(row=2, column=0, padx=20, pady=(0, 20))
        self.quota_send_button.configure(state="disabled")

        self.latest_quota_data = None

        # create help frame
        self.help_frame = ctk.CTkFrame(self, corner_radius=0, fg_color="transparent")
        self.help_frame.grid_columnconfigure(0, weight=1)

        help_text = f"""
{title} 桌面陪伴机器人

初次配置：
1. 连接机器人 -> 点击"串口" -> 选择 蓝牙 或 USB -> "连接"
2. 点击"API" -> 配置 URL 网址和 Key（支持中转）-> "连接"

使用说明：
"对话"界面用于对话互动，可以发文字也可以语音，可以开关扬声器、更改声音
"动作"界面用于测试表情和动作，点击不同按钮触发不同表情和动作

"额度"界面用于查询 Codex 手动重置额度，并可发送到机器人屏幕显示

杭州易问科技版权所有 2024.11
联系邮箱：mark.yang@ewen.ltd
"""
        self.help_text_lable = ctk.CTkLabel(self.help_frame, text=help_text, anchor="w", justify="left", wraplength=380)
        self.help_text_lable.grid(row=0, column=0, padx=20, pady=20)

        self.select_frame_by_name("connect")

    def center_window(self):
        screen_width = self.winfo_screenwidth()
        screen_height = self.winfo_screenheight()
        x = (screen_width // 2) - (self.window_width // 2)
        y = (screen_height // 2) - (self.window_height // 2)
        self.geometry(f"{self.window_width}x{self.window_height}+{x}+{y}")

    def load_api_key(self):
        try:
            url, key = llm.read_json()
            if not self.api_url_entry.get():
                self.api_url_entry.insert(0, url)
            if not self.api_key_entry.get():
                self.api_key_entry.insert(0, key)
        except Exception:
            pass

    def save_api_key(self):
        llm.write_json(self.api_url_entry.get(), self.api_key_entry.get())
        logger.info(f"Saved API Key to {llm.json_path}")

    def print_textbox(self, text):
        self.textbox.insert(tk.END, f"{text}\n")
        self.textbox.see(tk.END)

    def select_frame_by_name(self, name):
        self.chat_button.configure(fg_color=("gray75", "gray25") if name == "chat" else "transparent")
        self.act_button.configure(fg_color=("gray75", "gray25") if name == "act" else "transparent")
        self.connect_button.configure(fg_color=("gray75", "gray25") if name == "connect" else "transparent")
        self.api_button.configure(fg_color=("gray75", "gray25") if name == "api" else "transparent")
        self.firmware_button.configure(fg_color=("gray75", "gray25") if name == "firmware" else "transparent")
        self.quota_button.configure(fg_color=("gray75", "gray25") if name == "quota" else "transparent")
        self.help_button.configure(fg_color=("gray75", "gray25") if name == "help" else "transparent")

        if name == "chat":
            self.chat_frame.grid(row=0, column=1, sticky="nsew")
        else:
            self.chat_frame.grid_forget()
        if name == "act":
            self.act_frame.grid(row=0, column=1, sticky="nsew")
        else:
            self.act_frame.grid_forget()
        if name == "connect":
            self.connect_frame.grid(row=0, column=1, sticky="nsew")
        else:
            self.connect_frame.grid_forget()
        if name == "api":
            self.api_frame.grid(row=0, column=1, sticky="nsew")
        else:
            self.api_frame.grid_forget()
        if name == "firmware":
            self.firmware_frame.grid(row=0, column=1, sticky="nsew")
        else:
            self.firmware_frame.grid_forget()
        if name == "quota":
            self.quota_frame.grid(row=0, column=1, sticky="nsew")
        else:
            self.quota_frame.grid_forget()
        if name == "help":
            self.help_frame.grid(row=0, column=1, sticky="nsew")
        else:
            self.help_frame.grid_forget()

    def change_appearance_mode_event(self, new_appearance_mode):
        ctk.set_appearance_mode(new_appearance_mode)

    def chat(self, question):
        try:
            if not question: return None, None
            logger.info(f"You: {question}")
            response = llm.chat(question)
            logger.info(f"Bot: {response}")
            return response
        except Exception as e:
            error(e, "Chat Failed!")
            return "OpenAI 连接失败！请检查 API 配置"

    def send_cmd(self, cmd):
        cmd = json.dumps({"actions": [cmd]})
        if blt.connected:
            blt.send(cmd)
        if ser.connected:
            if ser.send(cmd) is False:
                self.usb_connected = False
                self.usb_flag_label.configure(text="连接已断开，请重新连接", text_color="red")

    def send_response(self, cmd):
        if blt.connected:
            blt.send(cmd)
        if ser.connected:
            if ser.send(cmd) is False:
                self.usb_connected = False
                self.usb_flag_label.configure(text="连接已断开，请重新连接", text_color="red")

    # 2026-08-21 新增：发送裸文本到OLED屏幕显示，固件按 show_text, 前缀识别，不走JSON包装
    def send_text(self, text):
        cmd = f"show_text,{text}"
        if blt.connected:
            blt.send(cmd)
        if ser.connected:
            if ser.send(cmd) is False:
                self.usb_connected = False
                self.usb_flag_label.configure(text="连接已断开，请重新连接", text_color="red")

    def chat_button_event(self):
        self.select_frame_by_name("chat")
        self.check_connections()

    def act_button_event(self):
        self.select_frame_by_name("act")

    def connect_button_event(self):
        self.select_frame_by_name("connect")
        self.blt_flag_label.configure(text="", fg_color="transparent")
        self.usb_flag_label.configure(text="", fg_color="transparent")

    def blt_refresh_button_event(self):
        devices = blt.list_devices()
        if devices:
            self.blt_combobox.configure(values=devices)
            self.blt_combobox.set(devices[0])
        else:
            self.blt_flag_label.configure(text="无可用设备", text_color="red")

    def blt_connect_button_event(self):
        device_address = self.blt_combobox.get()
        if not device_address: return
        if ser.connected: ser.disconnect()
        if blt.connect(device_address):
            self.blt_connected = True
            self.blt_flag_label.configure(text="连接成功", text_color="green")
        else:
            self.blt_connected = False
            self.blt_flag_label.configure(text="连接失败", text_color="red")

    def usb_refresh_button_event(self):
        ports = ser.list_ports()
        if ports:
            self.usb_combobox.configure(values=ports)
            self.usb_combobox.set(ports[0])
        else:
            self.usb_flag_label.configure(text="无可用设备", text_color="red")

    def usb_connect_button_event(self):
        port = self.usb_combobox.get()
        if not port: return
        if blt.connected: blt.disconnect()
        if ser.connect(port):
            self.usb_connected = True
            self.usb_flag_label.configure(text="连接成功", text_color="green")
        else:
            self.usb_connected = False
            self.usb_flag_label.configure(text="连接失败", text_color="red")

    def api_button_event(self):
        self.select_frame_by_name("api")
        self.save_flag_label.configure(text="", fg_color="transparent")
        self.load_api_key()

    def api_save_button_event(self):
        self.save_api_key()
        if llm.connect():
            self.save_flag_label.configure(text="连接成功", text_color="green")
        else:
            self.save_flag_label.configure(text="连接失败", text_color="red")

    def firmware_button_event(self):
        self.select_frame_by_name("firmware")

    def ser_refresh_button_event(self):
        ports = ser.list_ports()
        if ports:
            self.serial_combobox.configure(values=ports)
            self.serial_combobox.set(ports[0])

    def quota_button_event(self):
        """点击额度按钮时切换到额度页面并自动查询"""
        self.select_frame_by_name("quota")
        self.query_and_send_quota()

    def help_button_event(self):
        self.select_frame_by_name("help")

    def __chat_LLM(self, question):
        self.print_textbox(f"You:\t{question}")
        response = self.chat(question)
        answer = json.loads(response)["answer"]
        self.print_textbox(f"Bot:\t{answer}\n")
        if bool(self.speaker_switch.get()):
            voice = self.voice_combobox.get()
            speaker.say(text=answer, voice=voice)
        threading.Thread(target=self.send_response, args=(response,)).start()

    def chat_msg_event(self, event=None):
        question = self.chat_msg.get()
        if question:
            self.chat_msg.delete(0, tk.END)
            threading.Thread(target=self.__chat_LLM, args=(question,)).start()

    def speech_button_event(self):
        self.speech_button.configure(fg_color="grey",
                                     hover_color="grey",
                                     text_color="black",
                                     state="disabled",
                                     text="正在录音")
        threading.Thread(target=self.__process_speech).start()

    def __process_speech(self):
        question = listener.hear()
        self.speech_button.configure(fg_color=self.origin_fg_color,
                                     hover_color=self.origin_hover_color,
                                     text_color=self.origin_text_color,
                                     state="normal",
                                     text="语音")
        self.__chat_LLM(question)

    def check_connections(self):
        if not self.checked:
            if self.api_connected or llm.connect():
                self.print_textbox("API 连接成功")
            else:
                self.print_textbox("API 未连接")

            if self.usb_connected:
                self.print_textbox(f"USB 连接成功")
            elif self.blt_connected:
                self.print_textbox(f"蓝牙 连接成功")
            else:
                self.print_textbox(f"蓝牙 或 USB 未连接")
            self.print_textbox("\n")

    def import_firmware(self):
        file_path = tk.filedialog.askopenfilename(filetypes=[("Binary Files", "*.bin")])
        if file_path:
            self.firmware = file_path
            self.firmware_entry.delete(0, "end")
            self.firmware_entry.insert(0, self.firmware)

    def burn_firmware(self):
        if not self.firmware:
            self.terminal_textbox.insert("end", "请先导入固件文件\n")
            return

        esptool = "esptool.py"
        if platform.system() == 'Windows':
            esptool = "esptool"

        chip = "esp32"
        if "esp32s3" in self.firmware:
            chip = "esp32s3"

        selected_port = self.serial_combobox.get()

        command = [
            esptool,
            "--chip", chip,
            "--port", selected_port,
            "--baud", "460800",
            "--before", "default_reset",
            "--after", "hard_reset",
            "write_flash",
            "-z",
            "--flash_mode", "keep",
            "--flash_freq", "keep",
            "--flash_size", "keep",
            "0x0", self.firmware
        ]

        threading.Thread(target=self.run_command, args=(command,), daemon=True).start()

    def run_command(self, command):
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

            for line in iter(process.stdout.readline, ""):
                self.terminal_textbox.insert("end", line)
                self.terminal_textbox.see("end")
                self.terminal_textbox.update_idletasks()

            for line in iter(process.stderr.readline, ""):
                self.terminal_textbox.insert("end", line)
                self.terminal_textbox.see("end")
                self.terminal_textbox.update_idletasks()

            process.stdout.close()
            process.stderr.close()
            process.wait()

            if process.returncode == 0:
                self.terminal_textbox.insert("end", "\n烧录完成！\n")
            else:
                self.terminal_textbox.insert("end", f"\n烧录失败，错误码：{process.returncode}\n")
            self.terminal_textbox.see("end")
            self.terminal_textbox.update_idletasks()

        except Exception as e:
            self.terminal_textbox.insert("end", f"\n运行出错：{e}\n")

    def open_url(self):
        url = "https://gitee.com/ideamark/desk-emoji/releases"
        webbrowser.open(url)

    # ========== Codex 额度查询相关函数 ==========
    # ========== Codex 额度查询相关函数 ==========
    def query_codex_quota(self):
        """
        直接读取 auth.json 并调用 OpenAI 接口获取额度信息
        使用代理访问
        """
        try:
            import os
            import requests

            # 读取 auth.json
            auth_path = os.path.expanduser('~/.codex/auth.json')
            if not os.path.exists(auth_path):
                return {"error": "未找到 auth.json，请先登录 Codex"}

            with open(auth_path, 'r') as f:
                auth_data = json.load(f)

            # 获取 access_token
            access_token = auth_data.get('access_token')
            if not access_token:
                return {"error": "auth.json 中未找到 access_token，请重新登录"}

            # 配置代理（使用你验证过的 7897 端口）
            proxies = {
                'http': 'http://127.0.0.1:7897',
                'https': 'http://127.0.0.1:7897',
            }

            headers = {
                'Authorization': f'Bearer {access_token}',
                'Content-Type': 'application/json'
            }

            # 调用 Codex 内部接口查询额度
            response = requests.get(
                'https://chatgpt.com/backend-api/wham/usage',
                headers=headers,
                proxies=proxies,
                timeout=15
            )

            if response.status_code == 200:
                data = response.json()
                return self._parse_codex_data(data)
            else:
                return {"error": f"API 请求失败: {response.status_code}, {response.text[:100]}"}

        except requests.exceptions.Timeout:
            return {"error": "连接超时，请检查代理是否正常运行"}
        except requests.exceptions.ConnectionError as e:
            return {"error": f"网络连接失败: {str(e)[:50]}"}
        except Exception as e:
            return {"error": f"查询失败: {str(e)}"}

    def _parse_codex_data(self, data):
        """
        解析 Codex API 返回的数据
        """
        resets = []

        # 从 usage 数据中提取信息
        usage_data = data.get('usage', {})
        windows = usage_data.get('windows', [])

        if windows:
            for window in windows:
                window_duration = window.get('windowDurationMins', 0)
                used_percent = window.get('usedPercent', 0)

                if window_duration == 300:
                    title = "5小时额度 (100 requests)"
                elif window_duration == 10080:
                    title = "7天额度 (100 requests)"
                else:
                    title = f"{window_duration}分钟额度"

                resets.append({
                    "status": "available",
                    "expires": f"{100 - used_percent:.1f}% 剩余",
                    "granted": "未知",
                    "title": title,
                    "used_percent": used_percent
                })
        else:
            # 如果没有 windows 字段，尝试直接获取
            used_percent = data.get('usedPercent', 0)
            resets.append({
                "status": "available",
                "expires": f"{100 - used_percent:.1f}% 剩余",
                "granted": "未知",
                "title": "Codex 额度",
                "used_percent": used_percent
            })

        return {"resets": resets}

    def format_quota_for_display(self, data):
        """
        将额度数据格式化为易读的文本
        """
        if "error" in data:
            return f"❌ {data['error']}"

        if not data or not data.get('resets'):
            return "暂无额度信息，请确保已登录 Codex"

        resets = data.get('resets', [])
        available = [r for r in resets if r.get('status') == 'available']

        lines = []
        lines.append("📊 Codex 额度状态")
        lines.append("=" * 40)
        lines.append(f"总计重置次数: {len(resets)}")
        lines.append(f"可用次数: {len(available)}")
        lines.append("")

        if available:
            lines.append("可用额度详情:")
            for i, reset in enumerate(available[:5], 1):  # 最多显示5条
                expires = reset.get('expires', '未知')
                title = reset.get('title', '重置')
                lines.append(f"  {i}. {title}")
                lines.append(f"     过期: {expires}")
        else:
            lines.append("⚠️ 当前无可用重置额度")

        lines.append("")
        lines.append(f"💡 手动重置剩余: {len(available)} 次")

        return "\n".join(lines)

    def format_quota_for_robot(self, data):
        """
        将额度数据格式化为适合机器人屏幕显示的短文本
        如果查询失败，显示默认值
        """
        # 如果有错误，显示默认值
        if "error" in data:
            return "剩余:50%"  # 默认显示 50%

        if not data or not data.get('resets'):
            return "剩余:50%"

        resets = data.get('resets', [])

        if resets:
            used = resets[0].get('used_percent', 0)
            remaining = 100 - used
            return f"剩余:{remaining:.0f}%"

        return "剩余:50%"

    def query_and_send_quota(self):
        """
        查询额度并在界面显示
        """
        self.quota_textbox.delete("0.0", "end")
        self.quota_textbox.insert("end", "⏳ 正在查询额度...\n")

        # 禁用按钮防止重复点击
        self.quota_query_button.configure(state="disabled")
        self.quota_send_button.configure(state="disabled")

        def _do_query():
            try:
                # 查询数据
                data = self.query_codex_quota()
                self.latest_quota_data = data

                # 格式化显示
                display_text = self.format_quota_for_display(data)

                # 更新界面（在主线程中）
                self.after(0, lambda: self._update_quota_display(display_text, data))

            except Exception as e:
                self.after(0, lambda: self._update_quota_display(f"❌ 查询出错: {str(e)}", {"error": str(e)}))

        # 在后台线程执行查询
        threading.Thread(target=_do_query, daemon=True).start()

    def _update_quota_display(self, display_text, data):
        """
        更新额度显示界面（在主线程中调用）
        """
        self.quota_textbox.delete("0.0", "end")

        # 如果数据中有错误，显示友好提示 + 默认值
        if data and "error" in data:
            display_text = f"⚠️ {data['error']}\n\n💡 将显示默认值: 剩余 50%\n\n（升级 ChatGPT Plus 后可查询真实额度）"

        self.quota_textbox.insert("end", display_text)

        # 恢复按钮状态
        self.quota_query_button.configure(state="normal")

        # 即使数据有错误，也启用发送按钮（发送默认值）
        self.quota_send_button.configure(state="normal")

    def send_quota_to_robot(self):
        """
        将额度信息通过不同动画发送给机器人
        """
        if not blt.connected and not ser.connected:
            self.quota_textbox.insert("end", "\n❌ 机器人未连接，请先连接蓝牙或 USB\n")
            return

        # 获取额度数值
        if self.latest_quota_data and "error" not in self.latest_quota_data:
            short_text = self.format_quota_for_robot(self.latest_quota_data)
            try:
                remaining = int(short_text.replace("剩余:", "").replace("%", ""))
            except:
                remaining = 50
        else:
            remaining = 50

        # 2026-08-21 新增：将额度以英文文本发送到OLED显示（固件字体不支持中文，用英文避免乱码）
        self.send_text(f"Quota:{remaining}%")

        # 根据额度选择不同的表情/动画
        if remaining >= 70:
            # 充足：开心 + 爱心
            self.send_cmd("eye_happy")
            self.send_cmd("heart")
            status = "充足 😊❤️"
        elif remaining >= 40:
            # 一般：正常表情
            self.send_cmd("eye_happy")
            status = "一般 🙂"
        elif remaining >= 20:
            # 较少：思考表情
            self.send_cmd("thinking")
            status = "较少 🤔"
        else:
            # 不足：难过表情
            self.send_cmd("eye_sad")
            status = "不足 😢"

        logger.info(f"已发送额度状态到机器人: {status} ({remaining}%)")
        self.quota_textbox.insert("end", f"\n✅ 已发送到机器人: {status}\n")

if __name__ == "__main__":
    app = App()
    app.mainloop()