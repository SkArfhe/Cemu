package info.cemu.cemu.nativeinterface

import androidx.annotation.Keep
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.text.Normalizer

object NativeSwkbd {

    object SwkbdState {
        data class State(
            val text: String,
            val maxLength: Int,
        )

        private val _data = MutableStateFlow<State?>(null)
        val data = _data.asStateFlow()

        // Besides ASCII, accept the accented Latin-1 letters (á, ã, ç, é, ñ, ü...).
        // × (U+00D7) and ÷ (U+00F7) are symbols, not letters, and stay excluded.
        private val BlacklistedCharactersRegex =
            Regex("[^\\da-zA-Z\\u00C0-\\u00D6\\u00D8-\\u00F6\\u00F8-\\u00FF \\-/;:',.?!#\\[\\]$%^&*()_@\\\\<>+=]")

        fun updateText(text: String) {
            val currentState = _data.value ?: return

            // Some keyboards send "e" + combining accent; compose it into a single "é"
            // so it is not stripped by the filter below.
            val composedText = Normalizer.normalize(text, Normalizer.Form.NFC)
            val numChars =
                if (currentState.maxLength > 0) currentState.maxLength else composedText.length
            val newText = BlacklistedCharactersRegex.replace(composedText, "").take(numChars)
            _data.value = currentState.copy(
                text = newText
            )

            onTextChanged(newText)
        }

        @Keep
        @JvmStatic
        @Suppress("unused")
        private fun makeVisible(initialText: String?, maxLength: Int) {
            _data.value = State(initialText ?: "", maxLength)
        }

        @Keep
        @JvmStatic
        @Suppress("unused")
        private fun hide() {
            _data.value = null
        }
    }

    @JvmStatic
    external fun initializeSwkbd()

    @JvmStatic
    private external fun onTextChanged(text: String?)

    @JvmStatic
    external fun onFinishedInputEdit()
}
