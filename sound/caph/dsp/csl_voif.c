/******************************************************************************
*
* Copyright 2009 - 2012  Broadcom Corporation
*
*  Unless you and Broadcom execute a separate written software license
*  agreement governing use of this software, this software is licensed to you
*  under the terms of the GNU General Public License version 2 (the GPL),
*  available at
*
*      http://www.broadcom.com/licenses/GPLv2.php
*
*  with the following added to such license:
*
*  As a special exception, the copyright holders of this software give you
*  permission to link this software with independent modules, and to copy and
*  distribute the resulting executable under terms of your choice, provided
*  that you also meet, for each linked independent module, the terms and
*  conditions of the license of that module.
*  An independent module is a module which is not derived from this software.
*  The special exception does not apply to any modifications of the software.
*
*  Notwithstanding the above, under no circumstances may you combine this
*  software in any way with any other Broadcom software provided under a
*  license other than the GPL, without Broadcom's express prior written
*  consent.
*
******************************************************************************/
/**
*
*   @file   csl_voif.c
*
*   @brief  This file contains DSP VoIF interface
*
**********************************************************************/
#include "mobcom_types.h"
#include "shared.h"
#include "csl_voif.h"

/*********************************************************************/
/**
*
*   CSL_GetULVoIFBuffer gets pointer to UL VoIF buffer
*
*   @return   Int16*						uplink VoIF buffer
*
**********************************************************************/
Int16 *CSL_GetULVoIFBuffer(UInt16 sampleCount,
			   UInt16 ulBufferIndex, UInt8 opMode)
{
	if (opMode == 0x3)
		return &vp_shared_mem->
		    shared_voif_UL_buffer[ulBufferIndex * sampleCount];
	else
		return &vp_shared_mem->shared_voif_UL_buffer[0];

}

/*********************************************************************/
/**
*
*   CSL_GetDLVoIFBuffer gets pointer to DL VoIF buffer
*
*   @param    sampleCount		(in)		number of samples
*   @param    dlBufferIndex		(in)		downlink ping-pong buffer index
*   @return   Int16*						downlink VoIF buffer
*
**********************************************************************/
Int16 *CSL_GetDLVoIFBuffer(UInt16 sampleCount, UInt16 dlBufferIndex)
{
	return &vp_shared_mem->
	    shared_voif_DL_buffer[dlBufferIndex * sampleCount];

}
