package com.smarthelmet.app.bluetooth

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.Context
import com.smarthelmet.app.data.Contact
import kotlinx.coroutines.*
import java.io.IOException
import java.io.OutputStream
import java.util.UUID

/**
 * Handles a classic Bluetooth SPP connection to the ESP32 on the bike unit.
 * The ESP32 side should run BluetoothSerial with the same standard SPP UUID.
 *
 * Wire protocol (very simple, line-based, matches ESP32 sketch):
 *   "SYNC:Name1|Phone1;Name2|Phone2;...\n"
 * Bike ESP32 replies with "OK\n" on success.
 */
class HelmetBluetoothManager(private val context: Context) {

    // Standard Serial Port Profile UUID - do not change, must match ESP32 side
    private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

    private var socket: BluetoothSocket? = null
    private var outputStream: OutputStream? = null

    val isConnected: Boolean
        get() = socket?.isConnected == true

    @SuppressLint("MissingPermission")
    fun getPairedDevices(): List<BluetoothDevice> {
        val adapter = BluetoothAdapter.getDefaultAdapter() ?: return emptyList()
        return adapter.bondedDevices?.toList() ?: emptyList()
    }

    /**
     * Connects to a previously paired ESP32 device.
     * Must be called from a background thread/coroutine (IO dispatcher).
     */
    @SuppressLint("MissingPermission")
    suspend fun connect(device: BluetoothDevice): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            BluetoothAdapter.getDefaultAdapter()?.cancelDiscovery()
            val newSocket = device.createRfcommSocketToServiceRecord(SPP_UUID)
            newSocket.connect()
            socket = newSocket
            outputStream = newSocket.outputStream
            Result.success(Unit)
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    /**
     * Sends the full contact list to the bike unit so it can store it for GSM SMS alerts.
     */
    suspend fun syncContacts(contacts: List<Contact>): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val stream = outputStream ?: return@withContext Result.failure(
                IOException("Not connected to bike unit")
            )
            val payload = contacts.joinToString(separator = ";") { "${it.name}|${it.phone}" }
            val message = "SYNC:$payload\n"
            stream.write(message.toByteArray(Charsets.UTF_8))
            stream.flush()
            Result.success(Unit)
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    fun disconnect() {
        try {
            outputStream?.close()
            socket?.close()
        } catch (_: IOException) {
        } finally {
            outputStream = null
            socket = null
        }
    }
}
