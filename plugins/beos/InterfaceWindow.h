/*****************************************************************************
 * InterfaceWindow.h: BeOS interface window class prototype
 *****************************************************************************
 * Copyright (C) 1999, 2000, 2001 VideoLAN
 *
 * Authors: Jean-Marc Dressler <polux@via.ecp.fr>
 *          Tony Castley <tcastley@mail.powerup.com.au>
 *          Richard Shepherd <richard@rshepherd.demon.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

class SeekSlider;
class MediaSlider;

class CDMenu : public BMenu
{
public:
	CDMenu(const char *name);
	~CDMenu();
	void AttachedToWindow(void);
private:
	int GetCD(const char *directory);
};

class InterfaceWindow : public BWindow
{
public:
    InterfaceWindow( BRect frame, const char *name, intf_thread_t  *p_intf );
    ~InterfaceWindow();

    // standard window member
    virtual bool    QuitRequested();
    virtual void    MessageReceived(BMessage *message);
    
	
    intf_thread_t  *p_intf;
	MediaSlider * p_vol;
	SeekSlider * p_seek;
	BCheckBox * p_mute;
	sem_id	fScrubSem;
	bool	fSeeking;
	BFilePanel *file_panel;

};

class InterfaceView : public BView
{
public:
    InterfaceView();
    ~InterfaceView();

    virtual void    MessageReceived(BMessage *message);
};


class MediaSlider : public BSlider
{
public:
	MediaSlider(BRect frame,
				BMessage *message,
				int32 minValue,
				int32 maxValue);
	~MediaSlider();
	virtual void DrawThumb(void);
};
				

class SeekSlider : public MediaSlider
{
public:
	SeekSlider(BRect frame,
				InterfaceWindow *owner,
				int32 minValue,
				int32 maxValue,
				thumb_style thumbType = B_TRIANGLE_THUMB);

	~SeekSlider();
	
	virtual void MouseDown(BPoint);
	virtual void MouseUp(BPoint pt);
	virtual void MouseMoved(BPoint pt, uint32 c, const BMessage *m);
private:
	InterfaceWindow*	fOwner;	
	bool fMouseDown;
};
