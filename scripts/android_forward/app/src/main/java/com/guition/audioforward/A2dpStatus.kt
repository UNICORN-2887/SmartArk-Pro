package com.guition.audioforward

import android.bluetooth.BluetoothA2dp
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build

/**
 * 蓝牙 A2DP 状态监听：上报已连接耳机名（供 UI 显示）。
 * 配对与音频路由均由手机系统完成，本类只读状态。
 */
class A2dpStatus(
    private val context: Context,
    private val onChanged: (String) -> Unit
) {
    private var proxy: BluetoothA2dp? = null
    private var registered = false

    private val listener = object : BluetoothProfile.ServiceListener {
        override fun onServiceConnected(profile: Int, p: BluetoothProfile) {
            proxy = p as BluetoothA2dp
            refresh()
        }

        override fun onServiceDisconnected(profile: Int) {
            proxy = null
            onChanged("")
        }
    }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            refresh()
        }
    }

    fun start() {
        val adapter = BluetoothAdapter.getDefaultAdapter() ?: return
        if (!adapter.isEnabled) {
            onChanged("蓝牙未开启")
            return
        }
        try {
            adapter.getProfileProxy(context, listener, BluetoothProfile.A2DP)
        } catch (_: SecurityException) {
            return  // BLUETOOTH_CONNECT 未授权，等授权后重试由 MainActivity 触发
        }
        val filter = IntentFilter().apply {
            addAction(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED)
            addAction(BluetoothDevice.ACTION_ACL_CONNECTED)
            addAction(BluetoothDevice.ACTION_ACL_DISCONNECTED)
        }
        context.registerReceiver(receiver, filter)
        registered = true
    }

    fun stop() {
        if (registered) {
            try {
                context.unregisterReceiver(receiver)
            } catch (_: Exception) {
            }
            registered = false
        }
    }

    private fun refresh() {
        val p = proxy ?: return
        val devices: List<BluetoothDevice> = try {
            p.connectedDevices
        } catch (_: SecurityException) {
            emptyList()
        }
        onChanged(devices.joinToString("、") { it.name ?: it.address })
    }
}
