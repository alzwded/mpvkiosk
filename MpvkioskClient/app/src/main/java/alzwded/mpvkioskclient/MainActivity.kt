/*
 * Copyright (c) 2026, Vlad Meșco
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

package alzwded.mpvkioskclient

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.os.Bundle
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.EditText
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors

class MainActivity : AppCompatActivity() {

    private lateinit var sharedPreferences: SharedPreferences
    private lateinit var etServerUrl: EditText
    private lateinit var etUrlInput: EditText

    // This will hold the active URL in memory
    private var currentServerUrl: String = ""

    // A single thread executor to handle background network calls sequentially
    private val networkExecutor = Executors.newSingleThreadExecutor()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Initialize SharedPreferences
        sharedPreferences = getSharedPreferences("MediaClientPrefs", Context.MODE_PRIVATE)

        // Load the saved URL or default to the build-time configuration
        currentServerUrl = sharedPreferences.getString("SERVER_URL", BuildConfig.DEFAULT_SERVER_URL) ?: BuildConfig.DEFAULT_SERVER_URL

        etServerUrl = findViewById(R.id.etServerUrl)
        etUrlInput = findViewById(R.id.etUrlInput)

        // Populate the settings field with the saved URL
        etServerUrl.setText(currentServerUrl)

        setupButtons()

        // Guard intent handling to only occur on fresh launches,
        // preventing double-triggers on configuration changes (e.g. rotation)
        if (savedInstanceState == null) {
            handleIncomingIntent(intent)
        }
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        intent?.let { handleIncomingIntent(it) }
    }

    private fun handleIncomingIntent(intent: Intent) {
        if (intent.action == Intent.ACTION_SEND && intent.type == "text/plain") {
            val sharedText = intent.getStringExtra(Intent.EXTRA_TEXT)
            if (sharedText != null) {
                etUrlInput.setText(sharedText)
                triggerLoadFile(sharedText)
            }
        }
    }

    private fun setupButtons() {
        // Save Settings Button
        findViewById<Button>(R.id.btnSaveUrl).setOnClickListener {
            val newUrl = etServerUrl.text.toString().trim()
            if (newUrl.isNotBlank()) {
                currentServerUrl = newUrl
                sharedPreferences.edit().putString("SERVER_URL", newUrl).apply()
                Toast.makeText(this, "Server URL saved", Toast.LENGTH_SHORT).show()
            } else {
                Toast.makeText(this, "URL cannot be empty", Toast.LENGTH_SHORT).show()
            }
        }

        findViewById<Button>(R.id.btnLoad).setOnClickListener {
            val url = etUrlInput.text.toString()
            if (url.isNotBlank()) triggerLoadFile(url)
        }

        findViewById<Button>(R.id.btnPlayPause).setOnClickListener {
            sendPostRequest("/controls/playpause")
        }

        findViewById<Button>(R.id.btnStop).setOnClickListener {
            sendPostRequest("/controls/stop")
        }

        findViewById<Button>(R.id.btnPrev).setOnClickListener {
            sendPostRequest("/controls/prev")
        }

        findViewById<Button>(R.id.btnProgress).setOnClickListener {
            sendPostRequest("/controls/showprogress")
        }

        findViewById<Button>(R.id.btnNext).setOnClickListener {
            sendPostRequest("/controls/next")
        }

        findViewById<Button>(R.id.btnSeekBack10m).setOnClickListener {
            sendSeekRequest("-600")
        }

        findViewById<Button>(R.id.btnSeekBack1m).setOnClickListener {
            sendSeekRequest("-60")
        }

        findViewById<Button>(R.id.btnSeekBack).setOnClickListener {
            sendSeekRequest("-10")
        }

        findViewById<Button>(R.id.btnSeekForward).setOnClickListener {
            sendSeekRequest("10")
        }

        findViewById<Button>(R.id.btnSeekForward1m).setOnClickListener {
            sendSeekRequest("60")
        }

        findViewById<Button>(R.id.btnSeekForward10m).setOnClickListener {
            sendSeekRequest("600")
        }

        findViewById<Button>(R.id.btnBrowse).setOnClickListener {
            openBrowsePopup()
        }
    }

    private fun openBrowsePopup() {
        if (currentServerUrl.isBlank()) {
            showToastOnMain("Please save a Server URL first")
            return
        }

        val baseUrl = currentServerUrl.trimEnd('/')
        val defaultPath = BuildConfig.DEFAULT_SERVER_PATH
        val url = "$baseUrl/browse?path=$defaultPath"

        val webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView?, request: WebResourceRequest?): Boolean {
                    // Let the WebView load the URL
                    return false
                }
            }
            loadUrl(url)
        }

        AlertDialog.Builder(this)
            .setTitle("Browse Media")
            .setView(webView)
            .setPositiveButton("Close", null)
            .show()
    }

    private fun triggerLoadFile(path: String) {
        val params = mapOf("path" to path)
        sendPostRequest("/controls/loadfile", params)
        Toast.makeText(this, "Loading media...", Toast.LENGTH_SHORT).show()
    }

    private fun sendSeekRequest(value: String) {
        val params = mapOf("value" to value)
        sendPostRequest("/controls/seek", params)
    }

    /**
     * Executes an HTTP POST request on a background thread.
     */
    private fun sendPostRequest(endpoint: String, params: Map<String, String>? = null) {
        if (currentServerUrl.isBlank()) {
            showToastOnMain("Please save a Server URL first")
            return
        }

        networkExecutor.execute {
            var connection: HttpURLConnection? = null
            try {
                val baseUrl = currentServerUrl.trimEnd('/')
                val url = URL(baseUrl + endpoint)

                connection = url.openConnection() as HttpURLConnection
                connection.requestMethod = "POST"
                connection.connectTimeout = 5000
                connection.readTimeout = 5000
                // Explicitly use "close" to prevent the connection from being reused, 
                // which often causes EOFExceptions with 204 No Content responses.
                connection.setRequestProperty("Connection", "close")

                if (!params.isNullOrEmpty()) {
                    val postData = buildFormUrlEncodedString(params).toByteArray(Charsets.UTF_8)
                    connection.doOutput = true
                    connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
                    connection.setFixedLengthStreamingMode(postData.size)
                    connection.outputStream.use { os ->
                        os.write(postData)
                    }
                } else {
                    connection.setRequestProperty("Content-Length", "0")
                }

                val responseCode = connection.responseCode

                // 204 No Content is a successful response with no body.
                if (responseCode == HttpURLConnection.HTTP_NO_CONTENT) {
                    return@execute
                }

                if (responseCode !in 200..299) {
                    showToastOnMain("Server Error: $responseCode")
                } else {
                    // For other success codes, safely consume any response body.
                    try {
                        connection.inputStream.use { it.readBytes() }
                    } catch (e: Exception) {
                        // Ignore errors during stream consumption if the status was success.
                    }
                }
            } catch (e: Exception) {
                e.printStackTrace()
                val message = e.message ?: ""
                // Gracefully ignore "end of stream" or EOF errors which are common with 204 responses
                val isEofError = e is java.io.EOFException ||
                        message.contains("end of stream", ignoreCase = true) ||
                        message.contains("EOF", ignoreCase = true)

                if (!isEofError) {
                    showToastOnMain("Connection failed: $message")
                }
            } finally {
                connection?.disconnect()
            }
        }
    }

    private fun buildFormUrlEncodedString(params: Map<String, String>): String {
        val builder = java.lang.StringBuilder()
        for ((key, value) in params) {
            if (builder.isNotEmpty()) builder.append('&')
            builder.append(URLEncoder.encode(key, "UTF-8"))
            builder.append('=')
            builder.append(URLEncoder.encode(value, "UTF-8"))
        }
        return builder.toString()
    }

    private fun showToastOnMain(message: String) {
        runOnUiThread {
            Toast.makeText(this@MainActivity, message, Toast.LENGTH_SHORT).show()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        networkExecutor.shutdown()
    }
}