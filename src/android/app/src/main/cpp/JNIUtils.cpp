#include "JNIUtils.h"

static JavaVM* s_jvm = nullptr;

namespace JNIUtils
{
	std::string Utf16ToUtf8(const char16_t* data, size_t length)
	{
		std::string out;
		out.reserve(length);
		for (size_t i = 0; i < length; i++)
		{
			uint32 cp = data[i];
			if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length && data[i + 1] >= 0xDC00 && data[i + 1] <= 0xDFFF)
			{
				cp = 0x10000 + ((cp - 0xD800) << 10) + (data[i + 1] - 0xDC00);
				i++;
			}
			else if (cp >= 0xD800 && cp <= 0xDFFF)
			{
				cp = 0xFFFD; // unpaired surrogate
			}

			if (cp < 0x80)
			{
				out.push_back(static_cast<char>(cp));
			}
			else if (cp < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else if (cp < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
		}
		return out;
	}

	std::u16string Utf8ToUtf16(std::string_view str)
	{
		std::u16string out;
		out.reserve(str.size());
		const auto* bytes = reinterpret_cast<const uint8*>(str.data());
		const size_t length = str.size();
		size_t i = 0;
		while (i < length)
		{
			const uint8 lead = bytes[i];
			uint32 cp;
			size_t extra;
			uint32 minValue;
			if (lead < 0x80)
			{
				cp = lead, extra = 0, minValue = 0;
			}
			else if ((lead & 0xE0) == 0xC0)
			{
				cp = lead & 0x1F, extra = 1, minValue = 0x80;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				cp = lead & 0x0F, extra = 2, minValue = 0x800;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				cp = lead & 0x07, extra = 3, minValue = 0x10000;
			}
			else
			{
				out.push_back(u'\uFFFD'); // invalid lead byte
				i++;
				continue;
			}

			bool valid = i + extra < length;
			for (size_t k = 1; valid && k <= extra; k++)
			{
				const uint8 cont = bytes[i + k];
				if ((cont & 0xC0) != 0x80)
					valid = false;
				else
					cp = (cp << 6) | (cont & 0x3F);
			}
			if (!valid || cp < minValue || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			{
				out.push_back(u'\uFFFD');
				i++;
				continue;
			}
			i += extra + 1;

			if (cp >= 0x10000)
			{
				cp -= 0x10000;
				out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
				out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
			}
			else
			{
				out.push_back(static_cast<char16_t>(cp));
			}
		}
		return out;
	}

	void SetJavaVM(JavaVM* jvm)
	{
		s_jvm = jvm;
	}

	Scopedjobject::Scopedjobject(Scopedjobject&& other) noexcept
	{
		this->m_jobject = other.m_jobject;
		other.m_jobject = nullptr;
	}

	void Scopedjobject::DeleteReference()
	{
		if (m_jobject)
		{
			GetEnv()->DeleteGlobalRef(m_jobject);
			m_jobject = nullptr;
		}
	}

	Scopedjobject& Scopedjobject::operator=(Scopedjobject&& other) noexcept
	{
		if (this != &other)
		{
			DeleteReference();
			m_jobject = other.m_jobject;
			other.m_jobject = nullptr;
		}
		return *this;
	}

	jobject Scopedjobject::operator*() const
	{
		return m_jobject;
	}

	Scopedjobject::Scopedjobject(jobject obj)
	{
		if (obj)
			m_jobject = GetEnv()->NewGlobalRef(obj);
	}

	Scopedjobject::~Scopedjobject()
	{
		DeleteReference();
	}

	Scopedjclass::Scopedjclass(Scopedjclass&& other) noexcept
	{
		this->m_jclass = other.m_jclass;
		other.m_jclass = nullptr;
	}

	Scopedjclass::Scopedjclass(jclass javaClass)
	{
		if (javaClass)
			m_jclass = static_cast<jclass>(GetEnv()->NewGlobalRef(javaClass));
	}

	Scopedjclass& Scopedjclass::operator=(Scopedjclass&& other) noexcept
	{
		if (this != &other)
		{
			if (m_jclass)
				GetEnv()->DeleteGlobalRef(m_jclass);
			m_jclass = other.m_jclass;
			other.m_jclass = nullptr;
		}
		return *this;
	}

	Scopedjclass::Scopedjclass(const char* className)
	{
		JNIEnv* env = GetEnv();
		jclass tempObj = env->FindClass(className);
		m_jclass = static_cast<jclass>(env->NewGlobalRef(tempObj));
		env->DeleteLocalRef(tempObj);
	}

	Scopedjclass::~Scopedjclass()
	{
		if (m_jclass)
			GetEnv()->DeleteGlobalRef(m_jclass);
	}

	jclass Scopedjclass::operator*() const
	{
		return m_jclass;
	}

	Scopedjobject GetEnumValue(JNIEnv* env, const std::string& enumClassName, const std::string& enumName)
	{
		jclass enumClass = env->FindClass(enumClassName.c_str());
		jfieldID fieldID = env->GetStaticFieldID(enumClass, enumName.c_str(), ("L" + enumClassName + ";").c_str());
		jobject enumValue = env->GetStaticObjectField(enumClass, fieldID);
		env->DeleteLocalRef(enumClass);
		Scopedjobject enumObj = Scopedjobject(enumValue);
		env->DeleteLocalRef(enumValue);
		return enumObj;
	}

	JNIEnv* GetEnv()
	{
		thread_local static struct OwnedEnv
		{
			JNIEnv* env;
			jint result;

			OwnedEnv()
			{
				result = s_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);

				if (result == JNI_EDETACHED)
					s_jvm->AttachCurrentThread(&env, nullptr);
			}

			~OwnedEnv()
			{
				if (result == JNI_EDETACHED)
					s_jvm->DetachCurrentThread();
			}
		} owned;

		return owned.env;
	}
}; // namespace JNIUtils
