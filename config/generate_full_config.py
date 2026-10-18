#!/usr/bin/env python3
"""
Generate full board configuration for 240 DANSS detector boards.
Usage: python3 generate_full_config.py > config/board_map_full.json
"""

import json
import random

def generate_thermometer_id(board_num, channel_num):
    """Generate a unique 1-Wire thermometer ID."""
    # 1-Wire family code for DS18B20 is 0x28
    family = "28"
    # Generate unique serial based on board and channel
    serial = f"{board_num:04X}{channel_num:02X}{random.randint(0, 0xFFFF):04X}"
    # Calculate simple CRC (placeholder - in real system this would be proper CRC)
    crc = hex(sum(int(serial[i:i+2], 16) for i in range(0, len(serial), 2)) % 256)[2:].upper().zfill(2)
    return f"{family}{serial}{crc}"

def generate_cpu_id(board_num):
    """Generate CPU ID."""
    return f"CPU{board_num:04d}{random.randint(1000, 9999)}"

def generate_full_config(num_branches=8, boards_per_branch=30):
    """Generate configuration for all boards."""
    
    total_boards = num_branches * boards_per_branch
    
    config = {
        "system": {
            "poll_interval_sec": 300,
            "eeprom_check_interval_sec": 86400,
            "modbus_timeout_ms": 1000,
            "response_timeout_ms": 500,
            "max_retries": 3,
            "retry_delay_ms": 100
        },
        "branches": []
    }
    
    board_counter = 1
    
    for branch_id in range(num_branches):
        branch = {
            "id": branch_id,
            "device": f"/dev/ttyUSB{branch_id}",
            "baud_rate": 19200,
            "data_bits": 8,
            "parity": "N",
            "stop_bits": 1,
            "boards": []
        }
        
        for addr in range(1, boards_per_branch + 1):
            board = {
                "address": addr,
                "name": f"PMT-{branch_id+1:02d}-{addr:02d}",
                "enabled": True
            }
            branch["boards"].append(board)
            board_counter += 1
        
        config["branches"].append(branch)
    
    return config

def generate_full_eeprom_reference(num_branches=8, boards_per_branch=30):
    """Generate EEPROM reference data for all boards."""
    
    reference = {
        "description": "EEPROM reference data for all DANSS detector boards",
        "last_updated": "2025-01-18",
        "boards": []
    }
    
    board_num = 1
    
    for branch_id in range(num_branches):
        for addr in range(1, boards_per_branch + 1):
            board_ref = {
                "name": f"PMT-{branch_id+1:02d}-{addr:02d}",
                "modbus_address": addr,
                "branch_id": branch_id,
                "thermometer_ids": [
                    generate_thermometer_id(board_num, ch) for ch in range(12)
                ],
                "cpu_id": generate_cpu_id(board_num)
            }
            reference["boards"].append(board_ref)
            board_num += 1
    
    return reference

if __name__ == "__main__":
    import sys
    
    if len(sys.argv) > 1 and sys.argv[1] == "--eeprom":
        # Generate EEPROM reference
        eeprom_ref = generate_full_eeprom_reference(num_branches=8, boards_per_branch=30)
        print(json.dumps(eeprom_ref, indent=2))
    else:
        # Generate board map
        board_config = generate_full_config(num_branches=8, boards_per_branch=30)
        print(json.dumps(board_config, indent=2))
        
        # Print statistics
        total_boards = len([b for branch in board_config["branches"] for b in branch["boards"]])
        print(f"\n# Configuration generated:", file=sys.stderr)
        print(f"# Total branches: {len(board_config['branches'])}", file=sys.stderr)
        print(f"# Total boards: {total_boards}", file=sys.stderr)
        print(f"# Boards per branch: ~{total_boards // len(board_config['branches'])}", file=sys.stderr)
