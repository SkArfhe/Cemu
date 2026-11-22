package info.cemu.cemu.titlemanager.usecases

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import info.cemu.cemu.nativeinterface.NativeGameTitles
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

private typealias NativeCompressResult = NativeGameTitles.CompressResult

enum class CompressResult {
    FINISHED,
    ERROR,
}

enum class CompressionStage {
    STARTING,
    COLLECTING_FILES,
    COMPRESSING,
    FINALIZING,
}

data class CompressionProgress(
    val current: Long,
    val total: Long
)

class CompressTitleUseCase(private val scope: CoroutineScope) {
    private val _stage = MutableStateFlow<CompressionStage?>(null)
    val stage = _stage.asStateFlow()

    private val _progress = MutableStateFlow(CompressionProgress(0, 0))
    val progress = _progress.asStateFlow()

    private val _totalFileCount = MutableStateFlow(0)
    val totalFileCount = _totalFileCount.asStateFlow()

    private var compressJob: Job? = null
    private var progressJob: Job? = null

    // The output document of the running compression, deleted again if the
    // compression fails or is cancelled so no truncated .wua file is left behind.
    @Volatile
    private var outputDocument: Pair<Context, Uri>? = null

    @Volatile
    private var compressionFinished = false

    private fun deleteOutputDocument() {
        val (context, uri) = outputDocument ?: return
        outputDocument = null
        try {
            DocumentsContract.deleteDocument(context.contentResolver, uri)
        } catch (_: Exception) {
            // the provider may not support deleting; nothing else we can do
        }
    }

    fun cancel() {
        scope.launch(Dispatchers.IO) {
            // returns after the native worker has stopped and closed the file
            NativeGameTitles.cancelTitleCompression()
            if (!compressionFinished) {
                deleteOutputDocument()
            }
        }
    }

    fun compress(
        context: Context,
        uri: Uri,
        callback: (CompressResult) -> Unit
    ) {
        if (_stage.value != null) return

        // openFileDescriptor throws (instead of returning null) when the provider
        // cannot open the document, which used to crash the app
        val fd = try {
            context.contentResolver.openFileDescriptor(uri, "rw")
        } catch (_: Exception) {
            null
        }

        compressionFinished = false
        outputDocument = context.applicationContext to uri

        if (fd == null) {
            // the picker already created an empty document; don't leave it behind
            scope.launch(Dispatchers.IO) { deleteOutputDocument() }
            callback(CompressResult.ERROR)
            return
        }

        compressJob = scope.launch {
            _stage.value = CompressionStage.STARTING

            progressJob?.cancelAndJoin()

            try {
                progressJob = launch {
                    while (isActive) {
                        delay(500)
                        val currentProgress = NativeGameTitles.getCurrentProgressForCompression()

                        if (currentProgress == null) {
                            _stage.value = null
                            return@launch
                        }

                        val (current, total) = currentProgress
                        _progress.value = CompressionProgress(current, total)

                        _totalFileCount.value =
                            NativeGameTitles.getCurrentCompressionTotalFileCount()

                        val currentStage = NativeGameTitles.getCurrentCompressionStage()

                        if (currentStage == NativeGameTitles.CompressionStage.CANCELLED) {
                            _stage.value = null
                            return@launch
                        }

                        _stage.value = when (NativeGameTitles.getCurrentCompressionStage()) {
                            NativeGameTitles.CompressionStage.STARTING -> CompressionStage.STARTING
                            NativeGameTitles.CompressionStage.COLLECTING_FILES -> CompressionStage.COLLECTING_FILES
                            NativeGameTitles.CompressionStage.COMPRESSING -> CompressionStage.COMPRESSING
                            NativeGameTitles.CompressionStage.FINALIZING -> CompressionStage.FINALIZING
                            else -> CompressionStage.STARTING
                        }
                    }
                }

                NativeGameTitles.compressQueuedTitle(
                    fd = fd.detachFd(),
                    callback = { result ->
                        progressJob?.cancel()

                        when (result) {
                            NativeCompressResult.FINISHED -> {
                                compressionFinished = true
                                outputDocument = null
                                callback(CompressResult.FINISHED)
                            }

                            NativeCompressResult.ERROR -> {
                                scope.launch(Dispatchers.IO) { deleteOutputDocument() }
                                callback(CompressResult.ERROR)
                            }
                        }

                        _stage.value = null
                    }
                )
            } catch (_: Exception) {
                scope.launch(Dispatchers.IO) { deleteOutputDocument() }
                callback(CompressResult.ERROR)
                _stage.value = null
            }
        }
    }
}
