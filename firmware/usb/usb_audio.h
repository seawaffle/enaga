/* USB audio (usb_audio.c): endpoint addresses for usb.c's descriptor, and the audio interrupt's side. */
#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#define USB_AUDIO_EP_OUT 0x03                  /* isochronous, adaptive: the computer's audio */
#define USB_AUDIO_EP_IN  0x83                  /* isochronous, asynchronous: the unit's output */

#endif
