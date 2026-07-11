package com.smarthelmet.app.ui

import android.app.Application
import android.bluetooth.BluetoothDevice
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.smarthelmet.app.bluetooth.HelmetBluetoothManager
import com.smarthelmet.app.data.AppDatabase
import com.smarthelmet.app.data.Contact
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

sealed class SyncStatus {
    object Idle : SyncStatus()
    object Connecting : SyncStatus()
    object Sending : SyncStatus()
    object Success : SyncStatus()
    data class Error(val message: String) : SyncStatus()
}

class ContactsViewModel(application: Application) : AndroidViewModel(application) {

    private val dao = AppDatabase.getInstance(application).contactDao()
    private val bluetoothManager = HelmetBluetoothManager(application)

    val contacts: StateFlow<List<Contact>> =
        dao.getAll().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    private val _syncStatus = MutableStateFlow<SyncStatus>(SyncStatus.Idle)
    val syncStatus: StateFlow<SyncStatus> = _syncStatus

    fun addContact(name: String, phone: String) {
        if (name.isBlank() || phone.isBlank()) return
        viewModelScope.launch { dao.insert(Contact(name = name.trim(), phone = phone.trim())) }
    }

    fun deleteContact(contact: Contact) {
        viewModelScope.launch { dao.delete(contact) }
    }

    fun getPairedHelmetCandidates(): List<BluetoothDevice> = bluetoothManager.getPairedDevices()

    /** Connects to the chosen paired ESP32 device and pushes the current contact list. */
    fun connectAndSync(device: BluetoothDevice) {
        viewModelScope.launch {
            _syncStatus.value = SyncStatus.Connecting
            val connectResult = bluetoothManager.connect(device)
            if (connectResult.isFailure) {
                _syncStatus.value = SyncStatus.Error(
                    connectResult.exceptionOrNull()?.message ?: "Could not connect to helmet"
                )
                return@launch
            }

            _syncStatus.value = SyncStatus.Sending
            val sendResult = bluetoothManager.syncContacts(contacts.value)
            _syncStatus.value = if (sendResult.isSuccess) {
                SyncStatus.Success
            } else {
                SyncStatus.Error(sendResult.exceptionOrNull()?.message ?: "Failed to send contacts")
            }
        }
    }

    override fun onCleared() {
        super.onCleared()
        bluetoothManager.disconnect()
    }
}
