package com.smarthelmet.app.ui

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.smarthelmet.app.data.Contact

@SuppressLint("MissingPermission")
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DashboardScreen(viewModel: ContactsViewModel) {
    val contacts by viewModel.contacts.collectAsState()
    val syncStatus by viewModel.syncStatus.collectAsState()

    var name by remember { mutableStateOf("") }
    var phone by remember { mutableStateOf("") }
    var showDevicePicker by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(title = { Text("Smart Helmet - Emergency Contacts") })
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .padding(16.dp)
                .fillMaxSize()
        ) {
            Text("Add Emergency Contact", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(8.dp))

            OutlinedTextField(
                value = name,
                onValueChange = { name = it },
                label = { Text("Name") },
                modifier = Modifier.fillMaxWidth()
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = phone,
                onValueChange = { phone = it },
                label = { Text("Phone number") },
                modifier = Modifier.fillMaxWidth()
            )
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = {
                    viewModel.addContact(name, phone)
                    name = ""
                    phone = ""
                },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Add Contact")
            }

            Spacer(Modifier.height(24.dp))
            Text("Contacts (${contacts.size})", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(8.dp))

            LazyColumn(modifier = Modifier.weight(1f)) {
                items(contacts, key = { it.id }) { contact ->
                    ContactRow(contact = contact, onDelete = { viewModel.deleteContact(contact) })
                }
            }

            Spacer(Modifier.height(16.dp))

            Button(
                onClick = { showDevicePicker = true },
                modifier = Modifier.fillMaxWidth(),
                enabled = contacts.isNotEmpty()
            ) {
                Text("Sync Contacts to Helmet")
            }

            Spacer(Modifier.height(8.dp))
            SyncStatusLabel(syncStatus)
        }
    }

    if (showDevicePicker) {
        DevicePickerDialog(
            devices = viewModel.getPairedHelmetCandidates(),
            onDismiss = { showDevicePicker = false },
            onDeviceSelected = { device ->
                showDevicePicker = false
                viewModel.connectAndSync(device)
            }
        )
    }
}

@Composable
private fun ContactRow(contact: Contact, onDelete: () -> Unit) {
    Card(modifier = Modifier
        .fillMaxWidth()
        .padding(vertical = 4.dp)) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column {
                Text(contact.name, style = MaterialTheme.typography.bodyLarge)
                Text(contact.phone, style = MaterialTheme.typography.bodyMedium)
            }
            IconButton(onClick = onDelete) {
                Icon(Icons.Default.Delete, contentDescription = "Delete contact")
            }
        }
    }
}

@SuppressLint("MissingPermission")
@Composable
private fun DevicePickerDialog(
    devices: List<BluetoothDevice>,
    onDismiss: () -> Unit,
    onDeviceSelected: (BluetoothDevice) -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Select Helmet Device") },
        text = {
            if (devices.isEmpty()) {
                Text("No paired Bluetooth devices found. Pair with your helmet's ESP32 in phone Bluetooth settings first.")
            } else {
                Column {
                    devices.forEach { device ->
                        TextButton(onClick = { onDeviceSelected(device) }) {
                            Text(device.name ?: device.address)
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun SyncStatusLabel(status: SyncStatus) {
    val text = when (status) {
        is SyncStatus.Idle -> ""
        is SyncStatus.Connecting -> "Connecting to helmet..."
        is SyncStatus.Sending -> "Sending contacts..."
        is SyncStatus.Success -> "Contacts synced successfully!"
        is SyncStatus.Error -> "Error: ${status.message}"
    }
    if (text.isNotEmpty()) {
        Text(text, style = MaterialTheme.typography.bodyMedium)
    }
}
