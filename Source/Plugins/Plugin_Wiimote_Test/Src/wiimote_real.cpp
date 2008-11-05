// Copyright (C) 2003-2008 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "pluginspecs_wiimote.h"

#include <queue>

#include "common.h"
#include "thread.h"

#include "wiimote_hid.h"
#include "wiimote_emu.h"


#define WIIUSE_INTERNAL_H_INCLUDED
#define WIIUSE_COMPILE_LIB

#include "wiiuse_012/wiiuse.h"
#include "wiiuse_012/io.h"


extern SWiimoteInitialize g_WiimoteInitialize;
extern void __Log(int log, const char *format, ...);

namespace WiiMoteReal
{
#define MAX_WIIMOTES 1

	//******************************************************************************
	// Variable declarations
	//******************************************************************************

	wiimote_t** m_WiiMotesFromWiiUse = NULL;
	Common::Thread* g_pReadThread = NULL;
	Common::CriticalSection* g_pCriticalSection = NULL;
	bool g_Shutdown = false;
	
	class CWiiMote
	{
	public:

		CWiiMote(u8 _WiimoteNumber, wiimote_t* _pWiimote) 
			: m_WiimoteNumber(_WiimoteNumber)
			, m_pWiiMote(_pWiimote)
			, m_LastReportValid(false)
			, m_channelID(0)
		{
			wiiuse_set_leds(m_pWiiMote, WIIMOTE_LED_4);

#ifdef _WIN32
			// F|RES: i dunno if we really need this
			CancelIo(m_pWiiMote->dev_handle);
#endif
		}

		virtual ~CWiiMote() 
		{};

		// send raw HID data from the core to wiimote
		void SendData(u16 _channelID, const u8* _pData, u32 _Size)
		{
			m_channelID = _channelID;

			g_pCriticalSection->Enter();
			{
				SEvent WriteEvent;
				memcpy(WriteEvent.m_PayLoad, _pData+1, _Size-1);
				m_EventWriteQueue.push(WriteEvent);
			}
			g_pCriticalSection->Leave();
		}

		// read data from wiimote (but don't send it to the core, just filter and queue)
		void ReadData() 
		{
			g_pCriticalSection->Enter();

			if (!m_EventWriteQueue.empty())
			{
				SEvent& rEvent = m_EventWriteQueue.front();
				wiiuse_io_write(m_pWiiMote, (byte*)rEvent.m_PayLoad, MAX_PAYLOAD);
				m_EventWriteQueue.pop();
			}

			g_pCriticalSection->Leave();

			memset(m_pWiiMote->event_buf, 0, MAX_PAYLOAD);
			if (wiiuse_io_read(m_pWiiMote))
			{
				byte* pBuffer = m_pWiiMote->event_buf;

				// check if we have a channel (connection) if so save the data...
				if (m_channelID > 0)
				{
					g_pCriticalSection->Enter();

					// filter out reports
					if (pBuffer[0] >= 0x30) 
					{
						memcpy(m_LastReport.m_PayLoad, pBuffer, MAX_PAYLOAD);
						m_LastReportValid = true;
					}
					else
					{
						SEvent ImportantEvent;
						memcpy(ImportantEvent.m_PayLoad, pBuffer, MAX_PAYLOAD);
						m_EventReadQueue.push(ImportantEvent);
					}

					g_pCriticalSection->Leave();
				}
			}
		};

		// send queued data to the core
		void Update() 
		{
			g_pCriticalSection->Enter();

			if (m_EventReadQueue.empty())
			{
				if (m_LastReportValid)
					SendEvent(m_LastReport);
			}
			else
			{
				SendEvent(m_EventReadQueue.front());
				m_EventReadQueue.pop();
			}

			g_pCriticalSection->Leave();
		};

	private:

		struct SEvent 
		{
			SEvent()
			{
				memset(m_PayLoad, 0, MAX_PAYLOAD);
			}
			byte m_PayLoad[MAX_PAYLOAD];
		};
		typedef std::queue<SEvent> CEventQueue;

		u8 m_WiimoteNumber; // just for debugging
		u16 m_channelID;
		wiimote_t* m_pWiiMote;

		CEventQueue m_EventReadQueue;
		CEventQueue m_EventWriteQueue;
		bool m_LastReportValid;
		SEvent m_LastReport;

		void SendEvent(SEvent& _rEvent)
		{
			// we don't have an answer channel
			if (m_channelID == 0)
				return;

			// check event buffer;
			u8 Buffer[1024];
			u32 Offset = 0;
			hid_packet* pHidHeader = (hid_packet*)(Buffer + Offset);
			Offset += sizeof(hid_packet);
			pHidHeader->type = HID_TYPE_DATA;
			pHidHeader->param = HID_PARAM_INPUT;

			memcpy(&Buffer[Offset], _rEvent.m_PayLoad, MAX_PAYLOAD);
			Offset += MAX_PAYLOAD;

			g_WiimoteInitialize.pWiimoteInput(m_channelID, Buffer, Offset);
		}
	};

	int g_NumberOfWiiMotes;
	CWiiMote* g_WiiMotes[MAX_WIIMOTES];

	DWORD WINAPI ReadWiimote_ThreadFunc(void* arg);

	//******************************************************************************
	// Function Definitions 
	//******************************************************************************

	int Initialize()
	{
		memset(g_WiiMotes, 0, sizeof(CWiiMote*) * MAX_WIIMOTES);
		m_WiiMotesFromWiiUse = wiiuse_init(MAX_WIIMOTES);
		g_NumberOfWiiMotes= wiiuse_find(m_WiiMotesFromWiiUse, MAX_WIIMOTES, 5);

		for (int i=0; i<g_NumberOfWiiMotes; i++)
		{
			g_WiiMotes[i] = new CWiiMote(i+1, m_WiiMotesFromWiiUse[i]); 
		}

		if (g_NumberOfWiiMotes == 0)
			return 0;

		g_pCriticalSection = new Common::CriticalSection();
		g_pReadThread = new Common::Thread(ReadWiimote_ThreadFunc, NULL);

		return true;
	}

	void DoState(void* ptr, int mode)
	{}

	void Shutdown(void)
	{
		g_Shutdown = true;

		g_pReadThread->WaitForDeath();

		for (int i=0; i<g_NumberOfWiiMotes; i++)
		{
			delete g_WiiMotes[i];
			g_WiiMotes[i] = NULL;
		}

		delete g_pReadThread;
		g_pReadThread = NULL;
		delete g_pCriticalSection;
		g_pCriticalSection = NULL;
	}

	void InterruptChannel(u16 _channelID, const void* _pData, u32 _Size)
	{
		g_WiiMotes[0]->SendData(_channelID, (const u8*)_pData, _Size);
	}

	void ControlChannel(u16 _channelID, const void* _pData, u32 _Size)
	{
		g_WiiMotes[0]->SendData(_channelID, (const u8*)_pData, _Size);
	}
	
	void Update()
	{
		for (int i=0; i<g_NumberOfWiiMotes; i++)
		{
			g_WiiMotes[i]->Update();
		}
	}

	DWORD WINAPI ReadWiimote_ThreadFunc(void* arg)
	{
		while (!g_Shutdown)
		{
			for (int i=0; i<g_NumberOfWiiMotes; i++)
			{
				g_WiiMotes[i]->ReadData();
			}
		}
		return 0;
	}

}; // end of namespace

