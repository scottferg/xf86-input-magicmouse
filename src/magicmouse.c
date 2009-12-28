/*
 * Copyright 2009 Nico Nell
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <linux/input.h>
#include <linux/types.h>

#include <xf86_OSproc.h>

#include <unistd.h>

#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <xorgVersion.h>
#include <xkbsrv.h>


#ifdef HAVE_PROPERTIES
#include <xserver-properties.h>
/* 1.6 has properties, but no labels */
#ifdef AXIS_LABEL_PROP
#define HAVE_LABELS
#else
#undef HAVE_LABELS
#endif

#endif


#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <xorg-server.h>
#include <xorgVersion.h>
#include <xf86Module.h>
#include <X11/Xatom.h>

#include "magicmouse.h"

static InputInfoPtr MagicMousePreInit(InputDriverPtr  drv, IDevPtr dev, int flags);
static void MagicMouseUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static pointer MagicMousePlug(pointer module, pointer options, int *errmaj, int  *errmin);
static void MagicMouseUnplug(pointer p);
static void MagicMouseReadInput(InputInfoPtr pInfo);
static int MagicMouseControl(DeviceIntPtr    device,int what);
static int _magicmouse_init_buttons(DeviceIntPtr device);
static int _magicmouse_init_axes(DeviceIntPtr device);



_X_EXPORT InputDriverRec MAGICMOUSE = {
    1,
    "magicmouse",
    NULL,
    MagicMousePreInit,
    MagicMouseUnInit,
    NULL,
    0
};

static XF86ModuleVersionInfo MagicMouseVersionRec =
{
    "magicmouse",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR,
    PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData magicmouseModuleData =
{
    &MagicMouseVersionRec,
    &MagicMousePlug,
    &MagicMouseUnplug
};

static void
MagicMouseUnplug(pointer p)
{
};

static pointer
MagicMousePlug(pointer        module,
               pointer        options,
               int            *errmaj,
               int            *errmin)
{
    xf86AddInputDriver(&MAGICMOUSE, module, 0);
    return module;
};

static InputInfoPtr MagicMousePreInit(InputDriverPtr  drv,
                                      IDevPtr         dev,
                                      int             flags)
{
    InputInfoPtr           pInfo;
    MagicMouseDevicePtr    pMagic;


    if (!(pInfo = xf86AllocateInput(drv, 0)))
        return NULL;

    pMagic = xcalloc(1, sizeof(MagicMouseDeviceRec));
    if (!pMagic) {
        pInfo->private = NULL;
        xf86DeleteInput(pInfo, 0);
        return NULL;
    }

    pInfo->private = pMagic;
    pInfo->name = xstrdup(dev->identifier);
    pInfo->flags = 0;
    pInfo->type_name = XI_MOUSE; /* see XI.h */
    pInfo->conf_idev = dev;
    pInfo->read_input = MagicMouseReadInput; /* new data avl */
    pInfo->switch_mode = NULL; /* toggle absolute/relative mode */
    pInfo->device_control = MagicMouseControl; /* enable/disable dev */
    /* process driver specific options */

    /* TODO this will need to be changed */
    pMagic->device = xf86SetStrOption(dev->commonOptions,
                                      "Device",
                                      "/dev/random");

    xf86Msg(X_INFO, "%s: Using device %s.\n", pInfo->name, pMagic->device);

    /* process generic options */
    xf86CollectInputOptions(pInfo, NULL, NULL);
    xf86ProcessCommonOptions(pInfo, pInfo->options);
    /* Open sockets, init device files, etc. */
    SYSCALL(pInfo->fd = open(pMagic->device, O_RDWR | O_NONBLOCK));
    if (pInfo->fd == -1)
    {
        xf86Msg(X_ERROR, "%s: failed to open %s.",
                pInfo->name, pMagic->device);
        pInfo->private = NULL;
        xfree(pMagic);
        xf86DeleteInput(pInfo, 0);
        return NULL;
    }
    /* do more funky stuff */
    close(pInfo->fd);
    pInfo->fd = -1;
    pInfo->flags |= XI86_OPEN_ON_INIT;
    pInfo->flags |= XI86_CONFIGURED;
    return pInfo;
}

static void MagicMouseUnInit(InputDriverPtr drv,
                             InputInfoPtr   pInfo,
                             int            flags)
{
    MagicMouseDevicePtr     pMagic = pInfo->private;
    if (pMagic->device)
    {
        xfree(pMagic->device);
        pMagic->device = NULL;
        /* Common error - pInfo->private must be NULL or valid memoy before
         * passing into xf86DeleteInput */
        pInfo->private = NULL;
    }
    xf86DeleteInput(pInfo, 0);
}


static int
_magicmouse_init_buttons(DeviceIntPtr device)
{
    InputInfoPtr        pInfo = device->public.devicePrivate;
    MagicMouseDevicePtr     pMagic = pInfo->private;
    CARD8               *map;
    int                 i;
    int                 ret = Success;
    const int           num_buttons = 2;

    map = xcalloc(num_buttons, sizeof(CARD8));

    for (i = 0; i < num_buttons; i++)
        map[i] = i;

    pMagic->labels = xalloc(sizeof(Atom));

    if (!InitButtonClassDeviceStruct(device, num_buttons, pMagic->labels, map)) {
            xf86Msg(X_ERROR, "%s: Failed to register buttons.\n", pInfo->name);
            ret = BadAlloc;
    }

    xfree(map);
    return ret;
}

static void MagicMouseInitAxesLabels(MagicMouseDevicePtr pMagic, int natoms, Atom *atoms)
{
#ifdef HAVE_LABELS
    Atom atom;
    int axis;
    char **labels;
    int labels_len = 0;
    char *misc_label;

    labels     = rel_labels;
    labels_len = ArrayLength(rel_labels);
    misc_label = AXIS_LABEL_PROP_REL_MISC;

    memset(atoms, 0, natoms * sizeof(Atom));

    /* Now fill the ones we know */
    for (axis = 0; axis < labels_len; axis++)
    {
        if (pMagic->axis_map[axis] == -1)
            continue;

        atom = XIGetKnownProperty(labels[axis]);
        if (!atom) /* Should not happen */
            continue;

        atoms[pMagic->axis_map[axis]] = atom;
    }
#endif
}


static int
_magicmouse_init_axes(DeviceIntPtr device)
{
    InputInfoPtr        pInfo = device->public.devicePrivate;
    MagicMouseDevicePtr pMagic = pInfo->private;
    int                 i;
    const int           num_axes = 2;
    Atom                * atoms;

    pMagic->num_vals = num_axes;
    atoms = xalloc(pMagic->num_vals * sizeof(Atom));

    MagicMouseInitAxesLabels(pMagic, pMagic->num_vals, atoms);
    if (!InitValuatorClassDeviceStruct(device,
                                       num_axes,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                       atoms,
#endif	
                                       GetMotionHistorySize(),
                                       0))
        return BadAlloc;

    pInfo->dev->valuator->mode = Relative;
    if (!InitAbsoluteClassDeviceStruct(device))
            return BadAlloc;

    for (i = 0; i < pMagic->axes; i++) {
            xf86InitValuatorAxisStruct(device, i, *pMagic->labels, -1, -1, 1, 1, 1);
            xf86InitValuatorDefaults(device, i);
    }
    xfree(atoms);
    return Success;
}

static int MagicMouseControl(DeviceIntPtr    device,
                             int             what)
{
    InputInfoPtr        pInfo = device->public.devicePrivate;
    MagicMouseDevicePtr pMagic = pInfo->private;

    switch(what)
    {
        case DEVICE_INIT:
            _magicmouse_init_buttons(device);
            _magicmouse_init_axes(device);
            break;

        /* Switch device on.  Establish socket, start event delivery.  */
        case DEVICE_ON:
            xf86Msg(X_INFO, "%s: On.\n", pInfo->name);
            if (device->public.on)
                    break;

            SYSCALL(pInfo->fd = open(pMagic->device, O_RDONLY | O_NONBLOCK));
            if (pInfo->fd < 0)
            {
                xf86Msg(X_ERROR, "%s: cannot open device.\n", pInfo->name);
                return BadRequest;
            }

            xf86FlushInput(pInfo->fd);
            xf86AddEnabledDevice(pInfo);
            device->public.on = TRUE;
            break;
       case DEVICE_OFF:
            xf86Msg(X_INFO, "%s: Off.\n", pInfo->name);
            if (!device->public.on)
                break;
            xf86RemoveEnabledDevice(pInfo);
            close(pInfo->fd);
            pInfo->fd = -1;
            device->public.on = FALSE;
            break;
       case DEVICE_CLOSE:
            /* free what we have to free */
            break;
    }
    return Success;
}

static void MagicMouseReadInput(InputInfoPtr pInfo)
{
    char data;

    while(xf86WaitForInput(pInfo->fd, 0) > 0)
    {
        read(pInfo->fd, &data, 1);

        xf86PostMotionEvent(pInfo->dev,
                            0, /* is_absolute */
                            0, /* first_valuator */
                            1, /* num_valuators */
                            data);
    }
}

