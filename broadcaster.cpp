#include "stdafx.h"
#include "scanner.h"
#include "broadcaster.h"
#include "util.h"

//---------------------------------------------------------------------------
// ::BROADCAST
//---------------------------------------------------------------------------
Broadcaster::Broadcaster() 
: m_keyboardEnabled(false), m_windowsReadyToZoom(false), m_echo(false) 
{
	util::zeroMem(m_keyState);
	util::zeroMem(m_ctrlDown);
	initFilter();
	m_hThread = CreateThread( NULL, 0, inputThread, (LPVOID)this, 0, 0 );
}
//---------------------------------------------------------------------------
// ::~BROADCAST
//---------------------------------------------------------------------------
Broadcaster::~Broadcaster() {
	delete m_pWin;
}
//---------------------------------------------------------------------------
// BROADCAST
//---------------------------------------------------------------------------
int Broadcaster::broadcast(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static RAWINPUT raw;
	static UINT size = sizeof(raw);
	static DWORD& inputType = raw.header.dwType;
	static USHORT& scanCode = raw.data.keyboard.MakeCode;
	static USHORT& vKey = raw.data.keyboard.VKey;
	static USHORT& flags = raw.data.keyboard.Flags;
	static UINT& msg = raw.data.keyboard.Message;

	GetRawInputData((HRAWINPUT)lParam, RID_INPUT, (LPVOID)&raw, &size, sizeof(RAWINPUTHEADER));
	HWND curActiveWindow = GetForegroundWindow();
	Broadcaster& self = Broadcaster::getInstance();

//	if (self.m_echo && inputType == RIM_TYPEKEYBOARD) {
//		printf("VKEY: 0x%04X SCAN: 0x%04X FLAGS: 0x%04X\n", vKey,scanCode,flags);
//	}
	
	// update state
	if (inputType == RIM_TYPEKEYBOARD) {	
		if (self.rawKeyUp(flags)) {
			self.m_keyState[vKey] = 0; // key up
		} else if (self.rawKeyDown(flags)) {
			if (self.m_keyState[vKey]) {
				self.m_keyState[vKey] |= 0x80; // repeat
			} else {
				self.m_keyState[vKey] = 1; // key down 
			}
		}
	}

//	// Hotkeys 
//	if ( self.down(VK_CONTROL) && self.pressed(VK_OEM_6)) {
//		self.m_toBeRenamed = curActiveWindow;
//		SetForegroundWindow(GetConsoleWindow());
//		self.injectRename();
//		return 0;
//	}
//	if ( self.down(VK_CONTROL) && self.pressed(VK_OEM_PLUS) ) {
//		self.m_echo = !self.m_echo;
//		printf("echo %s\n", self.m_echo ? "ON" : "OFF");
//	}
	
	// zoom key '`'
	if (self.pressed(VK_OEM_8)) {
		static unsigned int pressed = 1;
		self.zoomSlave(!(++pressed%2));
	}

	Scanner& scanner = scanner.getInstance();
	Scanner::WINDOWLIST slaves;

	scanner.lock();
	scanner.getSlaves(slaves);

	// MMB=up arrow, Ctrl+MMB = down arrow, MB4 = strafe apart left arrow, Ctrl+MB4 = strafe together, right arrow
	if (inputType == RIM_TYPEMOUSE ) {
		unsigned short int& flags = raw.data.mouse.usButtonFlags;
		int btn = 
		 flags & RI_MOUSE_MIDDLE_BUTTON_DOWN ? MMB_DN :
		 flags & RI_MOUSE_MIDDLE_BUTTON_UP ? MMB_UP :
		 flags & RI_MOUSE_BUTTON_4_DOWN ? MB4_DN :
		 flags & RI_MOUSE_BUTTON_4_UP ? MB4_UP : NUM_MOUSESTATES;
			
		if (btn < self.NUM_MOUSESTATES) {
			for (size_t i=0; i<slaves.size(); ++i) {
				switch (btn) {
					case MMB_DN: {
						PostMessage(slaves[i]->hwnd,WM_KEYDOWN,(WPARAM)self.down(VK_CONTROL)?VK_DOWN:VK_UP,0);
						self.m_ctrlDown[btn] = self.down(VK_CONTROL);
						break;
					}
					case (MMB_UP): {
						PostMessage(slaves[i]->hwnd,WM_KEYUP,(WPARAM)self.m_ctrlDown[MMB_DN]?VK_DOWN:VK_UP,0);
						break;
					}
					case (MB4_DN): {
						PostMessage(slaves[i]->hwnd,WM_KEYDOWN,(WPARAM)self.down(VK_CONTROL)?VK_RIGHT:VK_LEFT,0);
						self.m_ctrlDown[btn] = self.down(VK_CONTROL);
						break;
					}
					case (MB4_UP): {
						PostMessage(slaves[i]->hwnd,WM_KEYUP,(WPARAM)self.m_ctrlDown[MB4_DN]?VK_RIGHT:VK_LEFT,0);
						break;
					}
				}
			}
			if (btn==MMB_UP || btn==MB4_UP) {
				self.m_ctrlDown[btn]=0;
			}
		}
	}

	if (scanner.master() && curActiveWindow == scanner.master()->hwnd && self.m_keyboardEnabled && inputType == RIM_TYPEKEYBOARD)  {
		BROADCAST_FILTER::iterator iter = self.m_filter.find(vKey);
		if (iter!=self.m_filter.end()) {
			UINT msg = self.rawKeyUp(flags) ? WM_KEYUP : self.rawKeyDown(flags) ? WM_KEYDOWN : 0;
			if (msg) {
				LPARAM lparam = self.pressed(vKey) ? 0 : self.repeated(vKey) ? 0x40000000 : 0xC0000000;
				lparam |= (scanCode<<16);
				for (size_t i=0; i<slaves.size(); ++i) {
					PostMessage(slaves[i]->hwnd,msg,(WPARAM)vKey,lparam);
				}
			}
		}
	}

	scanner.unlock();
	return 0; 
}
//---------------------------------------------------------------------------
// WINAPI BROADCAST::RAWINPUTTHREAD
//---------------------------------------------------------------------------
DWORD WINAPI Broadcaster::inputThread(LPVOID param)
{
	Broadcaster& self = Broadcaster::getInstance();
	self.m_pWin = new Win(HINST_THISCOMPONENT,"BroadcasterThread",0,0,10,10,0);
	self.m_pWin->addMsgHandler(WM_INPUT,broadcast);

	RAWINPUTDEVICE rid[2] = {
		{ HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, RIDEV_INPUTSINK|RIDEV_NOHOTKEYS, Broadcaster::getInstance().m_pWin->hwnd },
		{ HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, RIDEV_INPUTSINK, Broadcaster::getInstance().m_pWin->hwnd }
	};

	if (!RegisterRawInputDevices(rid,2,sizeof(rid[0]))) {
		printf("Warning: Broadcaster::inputThread failed to register raw input devices\n");
	}

	MSG msg;
	util::zeroMem(msg);

	while (msg.message != WM_QUIT) { 
		while ( PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE) == TRUE && msg.message != WM_QUIT) {
			if (GetMessage(&msg, NULL, 0, 0)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			} 
		}
	}

	printf("Input thread terminated\n");
	ExitThread(0);
}
//---------------------------------------------------------------------------
// BROADCAST::ZOOMSLAVE
//---------------------------------------------------------------------------
void Broadcaster::zoomSlave(bool zoom)
{
	if (!m_windowsReadyToZoom) {
		return;
	}

	POINT p;
	GetCursorPos(&p);

	Scanner& scanner = scanner.getInstance();
	Scanner::WINDOWLIST slaves;

	scanner.lock();
	scanner.getSlaves(slaves);
	HWND master = scanner.master()->hwnd;

	if (zoom) {
		// zoom window under cursor
		for (size_t i=0; i<slaves.size(); ++i) {
			RECT r;
			GetWindowRect(slaves[i]->hwnd, &r);
			bool mouseOver = ( p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom);
			if ( mouseOver ) {
				if (!slaves[i]->zoomed) {
					// zoom - this is just for the layout with slaves down RH side of screen
					util::Screen screen;
					RECT& pos = slaves[i]->pos;
					int w = screen.w*4/5;
					int h = screen.h*4/5;
					int x = screen.centre.x - w/2;
					int y = screen.centre.y - h/2;
					SetWindowPos(slaves[i]->hwnd,HWND_TOP,x,y,w,h,SWP_SHOWWINDOW);
					slaves[i]->zoomed = true;
					break;
				}
			}
		}
	} else {
		// unzoom 
		for (size_t i=0; i<slaves.size(); ++i) {
			if ( slaves[i]->zoomed) {
				RECT& pos = slaves[i]->pos;
				SetWindowPos(slaves[i]->hwnd,HWND_TOP,pos.left,pos.top,pos.right-pos.left,pos.bottom-pos.top,SWP_SHOWWINDOW);
				slaves[i]->zoomed = false;
			}
		}
	}

	scanner.unlock();
}
////---------------------------------------------------------------------------
//// BROADCAST::INJECTRENAME
////---------------------------------------------------------------------------
//void Broadcaster::injectRename()
//{
//	//TODO: should be in dispatcher
//	static struct { WORD vKey, scancode; } cmd = { 0x52, 0x13 }, newline = {VK_RETURN, 0x1C};
//	static INPUT_RECORD input[2] = {
//		{ KEY_EVENT, { TRUE,		0, cmd.vKey, cmd.scancode, {'r'}, 0 } },
//		{ KEY_EVENT, { TRUE,		0, newline.vKey, cmd.scancode, {newline.vKey}, 0 } },
//	};
//
//	DWORD count = 0;
//	HANDLE hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);
//	WriteConsoleInput(hConsoleIn,input,2,&count);
//}
//---------------------------------------------------------------------------
// BROADCAST::INITFILTER
//---------------------------------------------------------------------------
void Broadcaster::initFilter()
{
	m_filter.insert(BROADCAST_FILTER::value_type(VK_LEFT,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_UP,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_DOWN,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_RIGHT,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_TAB,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_SHIFT,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_CONTROL,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_MENU,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_SPACE,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_0,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_1,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_2,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_3,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_4,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_5,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_6,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_7,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_8,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_9,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_Q,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_E,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_R,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_T,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_Y,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_U,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_I,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_O,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_G,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_H,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_J,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_Z,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_X,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_V,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_N,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD0,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD1,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD2,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD3,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD4,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD5,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD6,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD7,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD8,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_NUMPAD9,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_MULTIPLY,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_ADD,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_SEPARATOR,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_SUBTRACT,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_DECIMAL,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_DIVIDE,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F1,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F2,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F3,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F4,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F5,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F6,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F7,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F8,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F9,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F10,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F11,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_F12,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_PLUS,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_COMMA,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_MINUS,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_PERIOD,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_1,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_2,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_3,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_4,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_5,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_6,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_7,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_8,1));
	m_filter.insert(BROADCAST_FILTER::value_type(VK_OEM_102,1));
}
