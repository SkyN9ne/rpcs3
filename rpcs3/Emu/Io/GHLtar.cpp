// Guitar Hero Live controller emulator

#include "stdafx.h"
#include "GHLtar.h"
#include "Emu/Cell/lv2/sys_usbd.h"
#include "Input/pad_thread.h"

LOG_CHANNEL(ghltar_log);

usb_device_ghltar::usb_device_ghltar(int controller_index, const std::array<u8, 7>& location)
	: usb_device_emulated(location), m_controller_index(controller_index)
{
	device        = UsbDescriptorNode(USB_DESCRIPTOR_DEVICE, UsbDeviceDescriptor{0x0200, 0x00, 0x00, 0x00, 0x20, 0x12BA, 0x074B, 0x0100, 0x01, 0x02, 0x00, 0x01});
	auto& config0 = device.add_node(UsbDescriptorNode(USB_DESCRIPTOR_CONFIG, UsbDeviceConfiguration{0x0029, 0x01, 0x01, 0x00, 0x80, 0x96}));
	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_INTERFACE, UsbDeviceInterface{0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00}));
	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_HID, UsbDeviceHID{0x0111, 0x00, 0x01, 0x22, 0x001d}));
	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_ENDPOINT, UsbDeviceEndpoint{0x81, 0x03, 0x0020, 0x01}));
	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_ENDPOINT, UsbDeviceEndpoint{0x01, 0x03, 0x0020, 0x01}));
}

usb_device_ghltar::~usb_device_ghltar()
{
}

void usb_device_ghltar::control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer)
{
	transfer->fake = true;

	// Control transfers are nearly instant
	switch (bmRequestType)
	{
		case 0x21:
			switch (bRequest)
			{
				case 0x09:
					// Do nothing here - not sure what it should do.
					break;
				default:
					ghltar_log.error("Unhandled Query Type: 0x%02X", buf[0]);
					break;
			}
			break;
		default:
			usb_device_emulated::control_transfer(bmRequestType, bRequest, wValue, wIndex, wLength, buf_size, buf, transfer);
			break;
	}
}

extern bool is_input_allowed();

void usb_device_ghltar::interrupt_transfer(u32 buf_size, u8* buf, u32 /*endpoint*/, UsbTransfer* transfer)
{
	transfer->fake            = true;
	transfer->expected_count  = buf_size;
	transfer->expected_result = HC_CC_NOERR;
	// Interrupt transfers are slow(6ms, TODO accurate measurement)
	// But make the emulated guitar go as fast as possible for better input behavior
	transfer->expected_time = get_timestamp();

	memset(buf, 0, buf_size);

	buf[0] = 0x00; // Frets
	// FRET HEXMASK:
	// 0x02 = B1
	// 0x04 = B2
	// 0x08 = B3
	// 0x01 = W1
	// 0x10 = W2
	// 0x20 = W3

	buf[1] = 0x00; // Buttons
	// BUTTONS HEXMASK:
	// 0x01 = Select/Hero Power
	// 0x02 = Start/Pause
	// 0x04 = GHTV Button
	// 0x10 = Sync Button

	buf[2] = 0x0F; // D-Pad
	// DPAD VALUES:
	// 0x00 = Up
	// 0x01 = Up-Left
	// 0x02 = Left
	// 0x03 = Left-Down
	// 0x04 = Down
	// 0x05 = Down-Right
	// 0x06 = Right
	// 0x07 = Up-Right
	// 0x0F = None

	buf[4] = 0x80; // Strummer

	buf[5]  = 0x80; // Hero Power (when buf[19] == 0x00 or 0xFF, set to that.)
	buf[6]  = 0x80; // Whammy
	buf[19] = 0x80; // Accelerometer

	buf[3]  = 0x80; // Unknown, always 0x80
	buf[22] = 0x01; // Unknown, always 0x01
	buf[24] = 0x02; // Unknown, always 0x02
	buf[26] = 0x02; // Unknown, always 0x02
	// buf[7] through buf[18] are always 0x00
	// buf[21]/[23]/[25] are also always 0x00

	if (!is_input_allowed())
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);
	const auto handler = pad::get_current_handler();
	const auto& pad    = handler->GetPads().at(m_controller_index);

	if (!(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
	{
		return;
	}

	for (const Button& button : pad->m_buttons)
	{
		if (!button.m_pressed)
		{
			continue;
		}

		if (button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
		{
			switch (button.m_outKeyCode)
			{
			case CELL_PAD_CTRL_SQUARE:
				buf[0] += 0x01; // W1
				break;
			case CELL_PAD_CTRL_CROSS:
				buf[0] += 0x02; // B1
				break;
			case CELL_PAD_CTRL_CIRCLE:
				buf[0] += 0x04; // B2
				break;
			case CELL_PAD_CTRL_TRIANGLE:
				buf[0] += 0x08; // B3
				break;
			case CELL_PAD_CTRL_R1:
				buf[0] += 0x20; // W3
				break;
			case CELL_PAD_CTRL_L1:
				buf[0] += 0x10; // W2
				break;
			default:
				break;
			}
		}
		else if (button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1)
		{
			switch (button.m_outKeyCode)
			{
			case CELL_PAD_CTRL_DOWN:
				buf[4] = 0xFF; // Strum Down
				break;
			case CELL_PAD_CTRL_UP:
				buf[4] = 0x00; // Strum Up
				break;
			case CELL_PAD_CTRL_LEFT:
				buf[2] = 0x02; // Left D-Pad (Unused)
				break;
			case CELL_PAD_CTRL_RIGHT:
				buf[2] = 0x06; // Right D-Pad (Unused)
				break;
			case CELL_PAD_CTRL_START:
				buf[1] += 0x02; // Pause
				break;
			case CELL_PAD_CTRL_SELECT:
				buf[1] += 0x01; // Hero Power
				break;
			case CELL_PAD_CTRL_L3:
				buf[1] += 0x04; // GHTV Button
				break;
			default:
				break;
			}
		}
	}
	for (const AnalogStick& stick : pad->m_sticks)
	{
		switch (stick.m_offset)
		{
		case CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y:
			buf[6] = ~(stick.m_value) + 0x01; // Whammy
			break;
		case CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X:
			buf[19] = static_cast<u8>(stick.m_value); // Tilt
			if (buf[19] >= 0xF0)
				buf[5] = 0xFF;
			if (buf[19] <= 0x10)
				buf[5] = 0x00;
			break;
		default:
			break;
		}
	}
}
