package com.smarthelmet.app.bluetooth

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.Context
import com.smarthelmet.app.data.Contact
import kotlinx.coroutines.*
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.IOException
import java.io.OutputStream
import java.util.UUID

/**
 * Handles a classic Bluetooth SPP connection to the ESP32 on the bike unit.
 * The ESP32 side should run BluetoothSerial with the same standard SPP UUID.
 *
 * Wire protocol (line-based, matches ESP32 sketch):
 *   "SYNC:Name1|Phone1;Name2|Phone2;...\n"
 * Bike ESP32 replies with "OK\n" on success or "ERR:..." on failure.
 */
class HelmetBluetoothManager(private val context: Context) {

    // Standard Serial Port Profile UUID - do not change, must match ESP32 side
    private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

    private var socket: BluetoothSocket? = null
    private var outputStream: OutputStream? = null
    private var reader: BufferedReader? = null

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
            reader = BufferedReader(InputStreamReader(newSocket.inputStream, Charsets.UTF_8))
            Result.success(Unit)
        } catch (e: IOException) {
            disconnect()
            Result.failure(e)
        }
    }

    /**
     * Sends the full contact list to the bike unit so it can store it for GSM SMS alerts.
     * Reads and verifies the response ("OK" vs "ERR:...").
     */
    suspend fun syncContacts(contacts: List<Contact>): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val stream = outputStream ?: return@withContext Result.failure(
                IOException("Not connected to bike unit")
            )
            val inReader = reader ?: return@withContext Result.failure(
                IOException("Input stream not initialized")
            )

            val payload = contacts.joinToString(separator = ";") { "${it.name}|${it.phone}" }
            val message = "SYNC:$payload\n"
            stream.write(message.toByteArray(Charsets.UTF_8))
            stream.flush()

            val response = inReader.readLine()?.trim()
            if (response == "OK") {
                Result.success(Unit)
            } else if (response != null && response.startsWith("ERR:")) {
                Result.failure(IOException("Bike unit returned error: $response"))
            } else if (response != null) {
                Result.failure(IOException("Unexpected response from bike unit: $response"))
            } else {
                Result.failure(IOException("No response received from bike unit"))
            }
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    fun disconnect() {
        try {
            reader?.close()
            outputStream?.close()
            socket?.close()
        } catch (_: IOException) {
        } finally {
            reader = null
            outputStream = null
            socket = null
        }
    }
}
