/*
 * Copyright 2012 Jared Boone
 * Copyright 2015-2024 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
 *
 * This file is part of lpc4370sdr (derived from HydraSDR, based on the HackRF project).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "usb_standard_request.h"

#include "usb.h"
#include "usb_type.h"
#include "usb_queue.h"

/* Shall be defined in user file usb_descriptor.c/.h */
extern uint8_t usb_descriptor_MSDescriptor[];
extern uint8_t usb_descriptor_ExtProps[];

uint16_t  USB_InterfaceStatus = 0; //Always 0x0000 value reseverd for future use
uint16_t  USB_DeviceStatus = 0; //Bit 0 = SelfPowerred, Bit=1 Remote Wakeup
uint8_t   USB_CurrentInterface = 0; //Bit 0 = SelfPowerred, Bit=1 Remote Wakeup
uint16_t  USB_EpStatus;

static uint_fast8_t usb_endpoint_number(const uint_fast8_t endpoint_address)
{
  return (endpoint_address & 0xF);
}

static const uint8_t* usb_endpoint_descriptor_from_address(
  const usb_device_t* const device,
  const uint_fast16_t endpoint_address)
{
  const usb_configuration_t* const configuration = device->configuration;
  if( configuration )
  {
    const uint8_t* descriptor = configuration->descriptor;
    while( descriptor[0] != 0 )
    {
      if( descriptor[1] == USB_DESCRIPTOR_TYPE_ENDPOINT )
      {
        if( descriptor[2] == endpoint_address ) {
          return descriptor;
        }
      }
      descriptor += descriptor[0];
    }
  }

  return 0;
}

const uint8_t* usb_endpoint_descriptor(const usb_endpoint_t* const endpoint)
{
  return usb_endpoint_descriptor_from_address(endpoint->device, endpoint->address);
}

/* The endpoint a request's wIndex names exists: endpoint 0, or one of the configuration's */
static bool usb_endpoint_address_valid(const usb_endpoint_t* const endpoint)
{
  const uint_fast16_t endpoint_address = endpoint->setup.index;
  return (endpoint_address & ~0x80) == 0
      || usb_endpoint_descriptor_from_address(endpoint->device, endpoint_address) != 0;
}

/* Interface requests: the configuration's only interface, 0 */
static bool usb_interface_valid(const usb_endpoint_t* const endpoint)
{
  return endpoint->device->configuration && endpoint->setup.index == 0;
}

uint_fast16_t usb_endpoint_descriptor_max_packet_size(const uint8_t* const endpoint_descriptor)
{
  return (endpoint_descriptor[5] << 8) | endpoint_descriptor[4];
}

usb_transfer_type_t usb_endpoint_descriptor_transfer_type(const uint8_t* const endpoint_descriptor)
{
  return (endpoint_descriptor[3] & 0x3);
}

void (*usb_configuration_changed_cb)(usb_device_t* const) = NULL;

void usb_set_configuration_changed_cb(void (*callback)(usb_device_t* const))
{
  usb_configuration_changed_cb = callback;
}

bool usb_set_configuration(
  usb_device_t* const device,
  const uint_fast8_t configuration_number)
{
  const usb_configuration_t* new_configuration = 0;
  if( configuration_number != 0 )
  {
    // Locate requested configuration.
    if( device->configurations )
    {
      usb_configuration_t** configurations = *(device->configurations);
      uint32_t i = 0;
      const usb_speed_t usb_speed_current = usb_speed(device);
      while( configurations[i] )
      {
        if( (configurations[i]->speed == usb_speed_current) &&
            (configurations[i]->number == configuration_number) )
        {
          new_configuration = configurations[i];
          break;
        }
        i++;
      }
    }

    // Requested configuration not found: request error.
    if( new_configuration == 0 ) {
      return false;
    }
  }

  if( new_configuration != device->configuration ) {
    // Configuration changed.
    device->configuration = new_configuration;
  }

  if (usb_configuration_changed_cb)
    usb_configuration_changed_cb(device);

  return true;
}

static usb_request_status_t usb_send_descriptor(
  usb_endpoint_t* const endpoint,
  const uint8_t* const descriptor_data)
{
  uint32_t descriptor_length = descriptor_data[0];
  if ( ( descriptor_data[1] == USB_DESCRIPTOR_TYPE_CONFIGURATION ) ||
        ( descriptor_data[1] == USB_DESCRIPTOR_TYPE_OTHER_SPEED_CONFIGURATION ) )
  {
    descriptor_length = (descriptor_data[3] << 8) | descriptor_data[2];
  }
  // We cast the const away but this shouldn't be a problem as this is a write transfer
  // (clamped to wLength, with the ZLP a shorter descriptor that ends on a packet needs)
  usb_control_reply(endpoint, (uint8_t* const) descriptor_data, descriptor_length);
  return USB_REQUEST_STATUS_OK;
}

static usb_request_status_t usb_send_descriptor_string(usb_endpoint_t* const endpoint)
{
  if ( endpoint->setup.value_l == 0xee)
  {
    return usb_send_descriptor(endpoint, usb_descriptor_MSDescriptor);
  }
  else
  {
    uint_fast8_t index = endpoint->setup.value_l;
    for( uint_fast8_t i=0; endpoint->device->descriptor_strings[i] != 0; i++ )
    {
      if( i == index ) {
        return usb_send_descriptor(endpoint, endpoint->device->descriptor_strings[i]);
      }
    }
  }

  return USB_REQUEST_STATUS_STALL;
}

/* OTHER_SPEED_CONFIGURATION: the other speed's configuration descriptor with its own type */
static uint8_t usb_other_speed_descriptor[64];

static usb_request_status_t usb_send_descriptor_config(
  usb_endpoint_t* const endpoint,
  usb_speed_t speed,
  const uint8_t config_num,
  const usb_descriptor_type_t type)
{
  usb_configuration_t** config = *(endpoint->device->configurations);
  unsigned int i = 0;
  for( ; *config != NULL; config++ )
  {
    if( (*config)->speed == speed)
    {
      if (i == config_num)
      {
        const uint8_t* descriptor = (*config)->descriptor;
        if( type == USB_DESCRIPTOR_TYPE_OTHER_SPEED_CONFIGURATION )
        {
          const uint32_t length = (descriptor[3] << 8) | descriptor[2];
          if( length > sizeof(usb_other_speed_descriptor) )
            return USB_REQUEST_STATUS_STALL;
          memcpy(usb_other_speed_descriptor, descriptor, length);
          usb_other_speed_descriptor[1] = USB_DESCRIPTOR_TYPE_OTHER_SPEED_CONFIGURATION;
          descriptor = usb_other_speed_descriptor;
        }
        return usb_send_descriptor(endpoint, descriptor);
      } else {
        i++;
      }
    }
  }
  return USB_REQUEST_STATUS_STALL;
}

static usb_request_status_t usb_standard_request_get_descriptor_setup(
  usb_endpoint_t* const endpoint)
{
  switch( endpoint->setup.value_h )
  {
    case USB_DESCRIPTOR_TYPE_DEVICE:
      return usb_send_descriptor(endpoint, endpoint->device->descriptor);

    case USB_DESCRIPTOR_TYPE_CONFIGURATION:
      // TODO: Duplicated code. Refactor.
      if( usb_speed(endpoint->device) == USB_SPEED_HIGH ) {
        return usb_send_descriptor_config(endpoint, USB_SPEED_HIGH, endpoint->setup.value_l, USB_DESCRIPTOR_TYPE_CONFIGURATION);
      } else {
        return usb_send_descriptor_config(endpoint, USB_SPEED_FULL, endpoint->setup.value_l, USB_DESCRIPTOR_TYPE_CONFIGURATION);
      }

    case USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER:
      return usb_send_descriptor(endpoint, endpoint->device->qualifier_descriptor);

    case USB_DESCRIPTOR_TYPE_OTHER_SPEED_CONFIGURATION:
      // TODO: Duplicated code. Refactor.
      if( usb_speed(endpoint->device) == USB_SPEED_HIGH ) {
        return usb_send_descriptor_config(endpoint, USB_SPEED_FULL, endpoint->setup.value_l, USB_DESCRIPTOR_TYPE_OTHER_SPEED_CONFIGURATION);
      } else {
        return usb_send_descriptor_config(endpoint, USB_SPEED_HIGH, endpoint->setup.value_l, USB_DESCRIPTOR_TYPE_OTHER_SPEED_CONFIGURATION);
      }

    case USB_DESCRIPTOR_TYPE_STRING:
      return usb_send_descriptor_string(endpoint);

    case USB_DESCRIPTOR_TYPE_INTERFACE:
    case USB_DESCRIPTOR_TYPE_ENDPOINT:
    default:
      return USB_REQUEST_STATUS_STALL;
  }
}

static usb_request_status_t usb_standard_request_get_descriptor(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  switch( stage )
  {
    case USB_TRANSFER_STAGE_SETUP:
      return usb_standard_request_get_descriptor_setup(endpoint);

    case USB_TRANSFER_STAGE_DATA:
    case USB_TRANSFER_STAGE_STATUS:
      return USB_REQUEST_STATUS_OK;

    default:
      return USB_REQUEST_STATUS_STALL;
  }
}

/*********************************************************************/

static usb_request_status_t usb_standard_request_set_address_setup(
  usb_endpoint_t* const endpoint)
{
  usb_set_address_deferred(endpoint->device, endpoint->setup.value_l);
  usb_transfer_schedule_ack(endpoint->in);
  return USB_REQUEST_STATUS_OK;
}

static usb_request_status_t usb_standard_request_set_address(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  switch( stage )
  {
    case USB_TRANSFER_STAGE_SETUP:
      return usb_standard_request_set_address_setup(endpoint);

    case USB_TRANSFER_STAGE_DATA:
    case USB_TRANSFER_STAGE_STATUS:
      /* NOTE: Not necessary to set address here, as DEVICEADR.USBADRA bit
       * will cause controller to automatically perform set address
       * operation on IN ACK.
       */
      return USB_REQUEST_STATUS_OK;

    default:
      return USB_REQUEST_STATUS_STALL;
  }
}

/*********************************************************************/

static usb_request_status_t usb_standard_request_set_configuration_setup(
  usb_endpoint_t* const endpoint)
{
  const uint8_t usb_configuration = endpoint->setup.value_l;
  /* Configuration 0 returns to the Address state: the device keeps its address (USB 2.0 9.4.7) */
  if( usb_set_configuration(endpoint->device, usb_configuration) )
  {
    usb_transfer_schedule_ack(endpoint->in);
    return USB_REQUEST_STATUS_OK;
  }
  else
  {
    return USB_REQUEST_STATUS_STALL;
  }
}

static usb_request_status_t usb_standard_request_set_configuration(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  switch( stage )
  {
    case USB_TRANSFER_STAGE_SETUP:
      return usb_standard_request_set_configuration_setup(endpoint);

    case USB_TRANSFER_STAGE_DATA:
    case USB_TRANSFER_STAGE_STATUS:
      return USB_REQUEST_STATUS_OK;

    default:
      return USB_REQUEST_STATUS_STALL;
  }
}

/*********************************************************************/

static usb_request_status_t usb_standard_request_get_configuration_setup(
  usb_endpoint_t* const endpoint)
{
  if( endpoint->setup.length == 1 )
  {
    endpoint->buffer[0] = 0;
    if( endpoint->device->configuration )
    {
      endpoint->buffer[0] = endpoint->device->configuration->number;
    }
    usb_transfer_schedule_block(endpoint->in, &endpoint->buffer, 1);
    usb_transfer_schedule_ack(endpoint->out);
    return USB_REQUEST_STATUS_OK;
  } else {
    return USB_REQUEST_STATUS_STALL;
  }
}

static usb_request_status_t usb_standard_request_get_configuration(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  switch( stage )
  {
    case USB_TRANSFER_STAGE_SETUP:
      return usb_standard_request_get_configuration_setup(endpoint);

    case USB_TRANSFER_STAGE_DATA:
    case USB_TRANSFER_STAGE_STATUS:
      return USB_REQUEST_STATUS_OK;

    default:
      return USB_REQUEST_STATUS_STALL;
  }
}
static usb_request_status_t usb_standard_request_get_status_endpoint(usb_endpoint_t* const endpoint)
{
  const uint_fast8_t endpoint_number = usb_endpoint_number(endpoint->setup.index);
  if( !usb_endpoint_address_valid(endpoint) )
    return USB_REQUEST_STATUS_STALL;
  /* The halt of the direction addressed */
  USB_EpStatus = (USB0_ENDPTCTRL(endpoint_number)
                  & ((endpoint->setup.index & 0x80) ? USB0_ENDPTCTRL_TXS : USB0_ENDPTCTRL_RXS)) ? 1 : 0;
  usb_control_reply(endpoint, &USB_EpStatus, 2);
  return USB_REQUEST_STATUS_OK;
}
static usb_request_status_t usb_standard_request_get_status(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  if (stage == USB_TRANSFER_STAGE_SETUP)
  {
    switch (endpoint->setup.request_type & USB_SETUP_REQUEST_TYPE_RECIPIENT_mask)
    {
      case USB_SETUP_REQUEST_TYPE_RECIPIENT_DEVICE:
        usb_control_reply(endpoint, &USB_DeviceStatus, 2);
        return USB_REQUEST_STATUS_OK;
        break;
      case USB_SETUP_REQUEST_TYPE_RECIPIENT_INTERFACE:
        if( !usb_interface_valid(endpoint) )
          return USB_REQUEST_STATUS_STALL;
        usb_control_reply(endpoint, &USB_InterfaceStatus, 2);
        return USB_REQUEST_STATUS_OK;
        break;
      case USB_SETUP_REQUEST_TYPE_RECIPIENT_ENDPOINT:
        return usb_standard_request_get_status_endpoint(endpoint);
        break;
    }
    return USB_REQUEST_STATUS_STALL;
  }
  return USB_REQUEST_STATUS_OK;
}

static usb_request_status_t usb_standard_request_get_interface(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  if (stage == USB_TRANSFER_STAGE_SETUP)
  {
    switch (endpoint->setup.request_type & USB_SETUP_REQUEST_TYPE_RECIPIENT_mask)
    {
      case USB_SETUP_REQUEST_TYPE_RECIPIENT_INTERFACE:
        if( !usb_interface_valid(endpoint) )
          return USB_REQUEST_STATUS_STALL;
        usb_control_reply(endpoint, &USB_CurrentInterface, 1);
        return USB_REQUEST_STATUS_OK;
        break;
    }
    return USB_REQUEST_STATUS_STALL;
  }
  return USB_REQUEST_STATUS_OK;
}


static usb_request_status_t usb_standard_request_set_interface(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  if (stage == USB_TRANSFER_STAGE_SETUP)
  {
    switch (endpoint->setup.request_type & USB_SETUP_REQUEST_TYPE_RECIPIENT_mask)
    {
      case USB_SETUP_REQUEST_TYPE_RECIPIENT_INTERFACE:
        /* Alternate setting 0 is the only one */
        if( !usb_interface_valid(endpoint) || endpoint->setup.value != 0 )
          return USB_REQUEST_STATUS_STALL;
        usb_transfer_schedule_ack(endpoint->in);
        return USB_REQUEST_STATUS_OK;
    }
    return USB_REQUEST_STATUS_STALL;
  }
  return USB_REQUEST_STATUS_OK;
}

/* ENDPOINT_HALT is the only feature: no remote wakeup, no test modes. Endpoint 0 takes it and
   ignores it, its stall ends with the next SETUP anyway. */
static usb_request_status_t usb_standard_request_feature_endpoint(
  usb_endpoint_t* const endpoint,
  const bool set)
{
  const uint_fast8_t endpoint_number = usb_endpoint_number(endpoint->setup.index);
  if( endpoint->setup.value != USB_FEATURE_ENDPOINT_HALT || !usb_endpoint_address_valid(endpoint) )
    return USB_REQUEST_STATUS_STALL;
  if( endpoint_number != 0 )
  {
    /* Only the direction addressed. ClearFeature(ENDPOINT_HALT) also resets the endpoint's data
       toggle to DATA0 (USB 2.0 9.4.5), as the host does on its side: otherwise the host drops the
       next packet half of the time */
    if( endpoint->setup.index & 0x80 ) /* IN endpoint address */
      USB0_ENDPTCTRL(endpoint_number) = set
        ? (USB0_ENDPTCTRL(endpoint_number) | USB0_ENDPTCTRL_TXS)
        : ((USB0_ENDPTCTRL(endpoint_number) & ~USB0_ENDPTCTRL_TXS) | USB0_ENDPTCTRL_TXR);
    else
      USB0_ENDPTCTRL(endpoint_number) = set
        ? (USB0_ENDPTCTRL(endpoint_number) | USB0_ENDPTCTRL_RXS)
        : ((USB0_ENDPTCTRL(endpoint_number) & ~USB0_ENDPTCTRL_RXS) | USB0_ENDPTCTRL_RXR);
  }
  usb_transfer_schedule_ack(endpoint->in);
  return USB_REQUEST_STATUS_OK;
}

static usb_request_status_t usb_standard_request_clear_feature(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  if (stage == USB_TRANSFER_STAGE_SETUP)
  {
    switch (endpoint->setup.request_type & USB_SETUP_REQUEST_TYPE_RECIPIENT_mask)
    {
      case USB_SETUP_REQUEST_TYPE_RECIPIENT_ENDPOINT:
        return usb_standard_request_feature_endpoint(endpoint, false);
        break;
    }
    return USB_REQUEST_STATUS_STALL;
  }
  return USB_REQUEST_STATUS_OK;
}

static usb_request_status_t usb_standard_request_set_feature(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  if (stage == USB_TRANSFER_STAGE_SETUP)
  {
    switch (endpoint->setup.request_type & USB_SETUP_REQUEST_TYPE_RECIPIENT_mask)
    {
      case USB_SETUP_REQUEST_TYPE_RECIPIENT_ENDPOINT:
        return usb_standard_request_feature_endpoint(endpoint, true);
        break;
    }
    return USB_REQUEST_STATUS_STALL;
  }
  return USB_REQUEST_STATUS_OK;
}

/*********************************************************************/

usb_request_status_t usb_standard_request(
  usb_endpoint_t* const endpoint,
  const usb_transfer_stage_t stage)
{
  switch( endpoint->setup.request )
  {
    case USB_STANDARD_REQUEST_GET_STATUS:
      return usb_standard_request_get_status(endpoint, stage);

    case USB_STANDARD_REQUEST_CLEAR_FEATURE:
      return usb_standard_request_clear_feature(endpoint, stage);

    case USB_STANDARD_REQUEST_SET_FEATURE:
      return usb_standard_request_set_feature(endpoint, stage);

  //USB_STANDARD_REQUEST_SET_DESCRIPTOR = 7,*/

    case USB_STANDARD_REQUEST_GET_INTERFACE:
      return usb_standard_request_get_interface(endpoint, stage);

    case USB_STANDARD_REQUEST_SET_INTERFACE:
      return usb_standard_request_set_interface(endpoint, stage);

    case USB_STANDARD_REQUEST_GET_DESCRIPTOR:
      return usb_standard_request_get_descriptor(endpoint, stage);

    case USB_STANDARD_REQUEST_SET_ADDRESS:
      return usb_standard_request_set_address(endpoint, stage);

    case USB_STANDARD_REQUEST_SET_CONFIGURATION:
      return usb_standard_request_set_configuration(endpoint, stage);

    case USB_STANDARD_REQUEST_GET_CONFIGURATION:
      return usb_standard_request_get_configuration(endpoint, stage);

    default:
      return USB_REQUEST_STATUS_STALL;
  }
}
