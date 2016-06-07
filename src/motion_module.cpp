// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#include "hw-monitor.h"
#include "motion_module.h"
#include <iostream>

using namespace rsimpl;

namespace rsimpl
{
    enum CX3_GrossTete_MonitorCommand : uint32_t
    {
        IRB = 0x01,     // Read from i2c ( 8x8 )
        IWB = 0x02,     // Write to i2c ( 8x8 )
        GVD = 0x03,     // Get Version and Date
        IAP_IRB = 0x04,     // Read from IAP i2c ( 8x8 )
        IAP_IWB = 0x05,     // Write to IAP i2c ( 8x8 )
        FRCNT = 0x06,     // Read frame counter
        GLD = 0x07,     // Get logger data
        GPW = 0x08,     // Write to GPIO
        GPR = 0x09,     // Read from GPIO
        MMPWR = 0x0A,     // Motion module power up/down
        DSPWR = 0x0B,     // DS4 power up/down
        EXT_TRIG = 0x0C,     // external trigger mode
        CX3FWUPD = 0x0D,     // FW update
        MM_ACTIVATE = 0x0E      // Motion Module activation
    };
}

motion_module_control::motion_module_control(uvc::device *device) : device_handle(device)
{
}

mm_state motion_module_state::requested_state(mm_request request, bool on) const
{
    int tmp = state;
    tmp += (int)request * (on ? 1 : -1);

    return (mm_state)tmp;
}

void motion_module_control::impose(mm_request request, bool on)
{
    mm_state new_state = state_handler.requested_state(request, on);

    if (motion_module_state::valid(new_state))
        enter_state(new_state);
    else
        throw std::logic_error("ABC");
}

void motion_module_control::enter_state(mm_state new_state)
{
    if (new_state == state_handler.state)
        return;

    // TODO refactor into state patters
    switch (state_handler.state)
    {
    case mm_idle:
        if (mm_streaming == new_state)
        {
            set_control(mm_video_output, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            //std::cout << "Switch from mm_idle to mm_streaming" << std::endl;
        }
        if (mm_eventing == new_state)
        {
            set_control(mm_video_output, true);
            set_control(mm_events_output, true);
            //std::cout << "Switch from mm_idle to mm_eventing" << std::endl;
        }
        break;
    case mm_streaming:
        if (mm_idle == new_state)
        {
            set_control(mm_video_output, false);
            //std::cout << "Switch from mm_eventing to mm_idle" << std::endl;
        }
        if (mm_full_load == new_state)
        {
            set_control(mm_events_output, true);
            //std::cout << "Switch from mm_streaming to mm_full_load" << std::endl;
        }
        break;
    case mm_eventing:
        if (mm_idle == new_state)
        {
            set_control(mm_events_output, false);
            //std::cout << "Switch from mm_eventing to mm_idle" << std::endl;
        }
        break;
    default:        // void
        break;
    }

    state_handler.state = new_state;
}

void motion_module_control::set_control(mm_request request, bool on)
{
    CX3_GrossTete_MonitorCommand cmd_opcode;
    switch (request)
    {
    case mm_video_output:
        cmd_opcode = CX3_GrossTete_MonitorCommand::MMPWR;
        break;
    case mm_events_output:
        cmd_opcode = CX3_GrossTete_MonitorCommand::MM_ACTIVATE;
        break;
    default:
        throw std::logic_error(to_string() << " unsupported control requested :" << (int)request << " valid range is [1,2]");
        break;
    }

    std::timed_mutex mutex;
    hw_mon::HWMonitorCommand cmd((uint8_t)cmd_opcode);
    cmd.Param1 = (on) ? 1 : 0;

    // Motion module will always use the auxillary USB handle (1) for
    rsimpl::hw_mon::perform_and_send_monitor_command(*device_handle, mutex, 1, cmd);
}

void motion_module_control::toggle_motion_module_power(bool on)
{
    // Apply user request, and update motion module controls if needed
    impose(mm_video_output, on);
}

void motion_module_control::toggle_motion_module_events(bool on)
{
    // Apply user request, and update motion module controls if needed
    impose(mm_events_output, on);
}


std::vector<motion_event> motion_module_parser::operator() (const unsigned char* data, const int& data_size)
{
    /* All sizes are in bytes*/
    const unsigned short motion_packet_header_size  = 8;
    const unsigned short imu_data_entries           = 4;
    const unsigned short imu_entry_size             = 12;
    const unsigned short non_imu_data_entries       = 8;        /* IMU SaS spec 3.3.2 */
    const unsigned short non_imu_entry_size         = 6;
    const unsigned short non_imu_data_offset        = motion_packet_header_size + (imu_data_entries * imu_entry_size);
    const unsigned short motion_packet_size         = non_imu_data_offset + (non_imu_data_entries * non_imu_entry_size);
    unsigned short packets = data_size / motion_packet_size;

    std::vector<motion_event> v;

    if (packets)
    {
        unsigned char *cur_packet = nullptr;

        for (uint8_t i = 0; i < packets; i++)
        {
            motion_event event_data = { 0 };

            cur_packet = (unsigned char*)data + (i*motion_packet_size);

            // extract packet info
            memcpy(&event_data.error_state, &cur_packet[0], sizeof(unsigned short));
            memcpy(&event_data.status, &cur_packet[2], sizeof(unsigned short));
            memcpy(&event_data.imu_entries_num, &cur_packet[4], sizeof(unsigned short));
            memcpy(&event_data.non_imu_entries_num, &cur_packet[6], sizeof(unsigned short));

            //std::cout << "New motion_packet arrived, imu_entries: " << event_data.imu_entries_num
            //             << ", non-imu_entries: " << event_data.non_imu_entries_num
            //          << std::endl;

            // Validate header input
            if ((event_data.imu_entries_num <= imu_data_entries) && (event_data.non_imu_entries_num <= non_imu_data_entries))
            {
                // Parse IMU entries
                for (uint8_t j = 0; j < event_data.imu_entries_num; j++)
                {
                    event_data.imu_packets[j] = parse_motion(&cur_packet[motion_packet_header_size + j*imu_entry_size]);
                }

                // Parse non-IMU entries
                for (uint8_t j = 0; j < event_data.non_imu_entries_num; j++)
                {
                    parse_timestamp(&cur_packet[non_imu_data_offset + j*non_imu_entry_size], event_data.non_imu_packets[j]);
                }

                v.push_back(std::move(event_data));
            }
        }
    }

    return v;
}

void motion_module_parser::parse_timestamp(const unsigned char * data, rs_timestamp_data &entry)
{
    // assuming msb ordering
    unsigned short  tmp = (data[1] << 8) | (data[0]);

    entry.source_id = rs_event_source(tmp & 0x7);       // bits [0:2] - source_id
    entry.frame_number = (tmp & 0x7fff) >> 3;           // bits [3-14] - frame num
    memcpy(&entry.timestamp, &data[2], sizeof(unsigned int));       // bits [16:47] - timestamp
}

rs_motion_data motion_module_parser::parse_motion(const unsigned char * data)
{
    // predefined motion devices ranges

    const static float gravity = 9.871f;
    const static float gyro_range = 2000.f;
    const static float gyro_transform_factor = (gyro_range * 3.141527f) / (180.f * 32768.f);

    const static float accel_range = 0.00195f;   // [-4..4]g
    const static float accelerator_transform_factor = accel_range * gravity;

    rs_motion_data entry;

    parse_timestamp(data, (rs_timestamp_data&)entry);

    entry.is_valid = (data[1] >> 7);          // Isolate bit[15]

    short tmp[3];
    memcpy(&tmp, &data[6], sizeof(short) * 3);

    unsigned data_shift = (RS_IMU_ACCEL == entry.timestamp_data.source_id) ? 4 : 0;

    for (int i = 0; i < 3; i++)                     // convert axis data to physical units (m/sec^2)
    {
        entry.axes[i] = float(tmp[i] >> data_shift);
        if (RS_IMU_ACCEL == entry.timestamp_data.source_id) entry.axes[i] *= accelerator_transform_factor;
        if (RS_IMU_GYRO == entry.timestamp_data.source_id) entry.axes[i] *= gyro_transform_factor;

        // TODO check and report invalid conversion requests
    }

    return entry;
}
