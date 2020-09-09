
/*

 Contour ShuttlePro v2 interface

 Copyright 2013 Eric Messick (FixedImagePhoto.com/Contact)

 Based on a version (c) 2006 Trammell Hudson <hudson@osresearch.net>

 which was in turn

 Based heavily on code by Arendt David <admin@prnet.org>

*/

#include "shuttle.h"

typedef struct input_event EV;

extern int debug_regex;
extern translation *default_translation;

unsigned short jogvalue = 0xffff;
int shuttlevalue = 0;
struct timeval last_shuttle;
int need_synthetic_shuttle;
Display *display;


void
initdisplay(void)
{
  int event, error, major, minor;

  display = XOpenDisplay(0);
  if (!display) {
    fprintf(stderr, "unable to open X display\n");
    exit(1);
  }
  if (!XTestQueryExtension(display, &event, &error, &major, &minor)) {
    fprintf(stderr, "Xtest extensions not supported\n");
    XCloseDisplay(display);
    exit(1);
  }
}

void
send_button(unsigned int button, int press)
{
  XTestFakeButtonEvent(display, button, press ? True : False, DELAY);
}

void
send_key(KeySym key, int press)
{
  KeyCode keycode;

  if (key >= XK_Button_1 && key <= XK_Scroll_Down) {
    send_button((unsigned int)key - XK_Button_0, press);
    return;
  }
  keycode = XKeysymToKeycode(display, key);
  XTestFakeKeyEvent(display, keycode, press ? True : False, DELAY);
}

stroke *
fetch_stroke(translation *tr, int kjs, int index)
{
  if (tr != NULL) {
    switch (kjs) {
    case KJS_SHUTTLE:
      return tr->shuttle[index];
    case KJS_JOG:
      return tr->jog[index];
    case KJS_KEY_UP:
      return tr->key_up[index];
    case KJS_KEY_DOWN:
    default:
      return tr->key_down[index];
    }
  }
  return NULL;
}

void
send_stroke_sequence(translation *tr, int kjs, int index)
{
  stroke *s;

  s = fetch_stroke(tr, kjs, index);
  if (s == NULL) {
    s = fetch_stroke(default_translation, kjs, index);
  }
  while (s) {
    send_key(s->keysym, s->press);
    s = s->next;
  }
  XFlush(display);
}

void
key(unsigned short code, unsigned int value, translation *tr)
{
  code -= EVENT_CODE_KEY1;

  if (code <= NUM_KEYS) {
    send_stroke_sequence(tr, value ? KJS_KEY_DOWN : KJS_KEY_UP, code);
  } else {
    fprintf(stderr, "key(%d, %d) out of range\n", code + EVENT_CODE_KEY1, value);
  }
}


void
shuttle(int value, translation *tr)
{
  if (value < -7 || value > 7) {
    fprintf(stderr, "shuttle(%d) out of range\n", value);
  } else {
    gettimeofday(&last_shuttle, 0);
    need_synthetic_shuttle = value != 0;
    // Treat shuttle commands as TRANSITIONS from one shuttle state to another
    if ( shuttlevalue < value ) 
      send_stroke_sequence(tr, KJS_SHUTTLE, abs(value)+7);
    else if ( shuttlevalue > value )
      send_stroke_sequence(tr, KJS_SHUTTLE, 7-abs(value));
    shuttlevalue = value;
  }
}

// Due to a bug (?) in the way Linux HID handles the ShuttlePro, the
// center position is not reported for the shuttle wheel.  Instead,
// a jog event is generated immediately when it returns.  We check to
// see if the time since the last shuttle was more than a few ms ago
// and generate a shuttle of 0 if so.
//
// Note, this fails if jogvalue happens to be 0, as we don't see that
// event either!
void
jog(unsigned int value, translation *tr)
{
  int direction;
  struct timeval now;
  struct timeval delta;

  // We should generate a synthetic event for the shuttle going
  // to the home position if we have not seen one recently
  if (need_synthetic_shuttle) {
    gettimeofday( &now, 0 );
    timersub( &now, &last_shuttle, &delta );

    if (delta.tv_sec >= 1 || delta.tv_usec >= 5000) {
      shuttle(0, tr);
      need_synthetic_shuttle = 0;
    }
  }

  if (jogvalue != 0xffff) {
    value = value & 0xff;
    direction = ((value - jogvalue) & 0x80) ? -1 : 1;
    while (jogvalue != value) {
      // driver fails to send an event when jogvalue == 0
      if (jogvalue != 0) {
	send_stroke_sequence(tr, KJS_JOG, direction > 0 ? 1 : 0);
      }
      jogvalue = (jogvalue + direction) & 0xff;
    }
  }
  jogvalue = value;
}

void
jogshuttle(unsigned short code, unsigned int value, translation *tr)
{
  switch (code) {
  case EVENT_CODE_JOG:
    jog(value, tr);
    break;
  case EVENT_CODE_SHUTTLE:
    shuttle(value, tr);
    break;
  case EVENT_CODE_SHUTTLE_HI_RES:
    break;
  default:
    fprintf(stderr, "jogshuttle(%d, %d) invalid code\n", code, value);
    break;
  }
}

char *
get_window_name(Window win)
{
  Atom prop = XInternAtom(display, "WM_NAME", False);
  Atom type;
  int form;
  unsigned long remain, len;
  unsigned char *list;

  if (XGetWindowProperty(display, win, prop, 0, 1024, False,
			 AnyPropertyType, &type, &form, &len, &remain,
			 &list) != Success) {
    fprintf(stderr, "XGetWindowProperty failed for window 0x%x\n", (int)win);
    return NULL;
  }

  return (char*)list;
}

char *
walk_window_tree(Window win)
{
  char *window_name;
  Window root = 0;
  Window parent;
  Window *children;
  unsigned int nchildren;

  while (win != root) {
    window_name = get_window_name(win);
    if (window_name != NULL) {
      return window_name;
    }
    if (XQueryTree(display, win, &root, &parent, &children, &nchildren)) {
      win = parent;
      XFree(children);
    } else {
      fprintf(stderr, "XQueryTree failed for window 0x%x\n", (int)win);
      return NULL;
    }
  }
  return NULL;
}

static Window last_focused_window = 0;
static translation *last_window_translation = NULL;

translation *
get_focused_window_translation()
{
  Window focus;
  int revert_to;
  char *window_name = NULL;
  char *name;

  XGetInputFocus(display, &focus, &revert_to);
  if (focus != last_focused_window) {
    last_focused_window = focus;
    window_name = walk_window_tree(focus);
    if (window_name == NULL) {
      name = "-- Unlabeled Window --";
    } else {
      name = window_name;
    }
    last_window_translation = get_translation(name);
    if (debug_regex) {
      if (last_window_translation != NULL) {
	printf("translation: %s for %s\n", last_window_translation->name, name);
      } else {
	printf("no translation found for %s\n", name);
      }
    }
    if (window_name != NULL) {
      XFree(window_name);
    }
  }
  return last_window_translation;
}

void
handle_event(EV ev)
{
  translation *tr = get_focused_window_translation();
  
  //fprintf(stderr, "event: (%d, %d, 0x%x)\n", ev.type, ev.code, ev.value);
  if (tr != NULL) {
    switch (ev.type) {
    case EVENT_TYPE_DONE:
    case EVENT_TYPE_ACTIVE_KEY:
      break;
    case EVENT_TYPE_KEY:
      key(ev.code, ev.value, tr);
      break;
    case EVENT_TYPE_JOGSHUTTLE:
      jogshuttle(ev.code, ev.value, tr);
      break;
    default:
      fprintf(stderr, "handle_event() invalid type code\n");
      break;
    }
  }
}


int
main(int argc, char **argv)
{
  EV ev;
  int nread;
  char *dev_name;
  int fd;
  int first_time = 1;

  if (argc != 2) {
    fprintf(stderr, "usage: shuttlepro <device>\n" );
    exit(1);
  }

  dev_name = argv[1];

  initdisplay();

  while (1) {
    fd = open(dev_name, O_RDONLY);
    if (fd < 0) {
      perror(dev_name);
      if (first_time) {
	exit(1);
      }
    } else {
      // Flag it as exclusive access
      if(ioctl( fd, EVIOCGRAB, 1 ) < 0) {
	perror( "evgrab ioctl" );
      } else {
	first_time = 0;
	while (1) {
	  nread = read(fd, &ev, sizeof(ev));
	  if (nread == sizeof(ev)) {
	    handle_event(ev);
	  } else {
	    if (nread < 0) {
	      perror("read event");
	      break;
	    } else {
	      fprintf(stderr, "short read: %d\n", nread);
	      break;
	    }
	  }
	}
      }
    }
    close(fd);
    sleep(1);
  }
}
