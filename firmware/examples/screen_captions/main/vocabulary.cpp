#include "vocabulary.h"

#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "transcript.h"

namespace vocabulary {
namespace {
constexpr const char* kPath = "/sd/STT Models/user_vocab.tsv";
EXT_RAM_BSS_ATTR hw1::transcript::Vocabulary gVocabulary;
EXT_RAM_BSS_ATTR hw1::stt::lm::CustomWords gWords;
}  // namespace

size_t load() {
  gVocabulary.clear();
  FILE* file = std::fopen(kPath, "r");
  if (!file) return 0;
  char line[160], error[128];
  bool ok = true;
  while (ok && std::fgets(line, sizeof(line), file)) {
    line[std::strcspn(line, "\r\n")] = 0;
    if (!line[0]) continue;
    char* spoken = std::strchr(line, '\t');
    if (spoken) *spoken++ = 0;
    ok = gVocabulary.put(line, spoken, error, sizeof(error));
    if (!ok) printf("Vocabulary line \"%s\" rejected: %s\n", line, error);
  }
  std::fclose(file);
  if (!ok) gVocabulary.clear();  // all or nothing: a half-read list would surprise more than none

  static char text[hw1::transcript::kMaxWords * (hw1::transcript::kSpokenBytes + 1) + 1];
  size_t used = 0;
  for (size_t i = 0; i < gVocabulary.count(); ++i) {
    const size_t n = std::strlen(gVocabulary.at(i).spoken);
    std::memcpy(text + used, gVocabulary.at(i).spoken, n);
    used += n;
    text[used++] = '\n';
  }
  gWords.parse(text, used);
  return gVocabulary.count();
}

const hw1::stt::lm::CustomWords* words() { return gWords.size() ? &gWords : nullptr; }

bool format(const char* raw, char* shown, size_t capacity) {
  return hw1::transcript::format(raw, gVocabulary, true, shown, capacity);
}
}  // namespace vocabulary
