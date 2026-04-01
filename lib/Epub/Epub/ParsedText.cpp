#include "ParsedText.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }

  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

bool isIndentAlignment(const CssTextAlign alignment) {
  return alignment == CssTextAlign::Justify || alignment == CssTextAlign::Left;
}

int positiveTextIndentPx(const BlockStyle& blockStyle) {
  return blockStyle.textIndent > 0 ? static_cast<int>(blockStyle.textIndent) : 0;
}

// Stable first-line indent policy:
// 1) Always respect a positive CSS text-indent.
// 2) If no positive CSS indent exists and the user enabled paragraphIndent, apply a fallback indent.
// 3) Never inject visible characters into the text stream.
int effectiveFirstLineIndentPx(const BlockStyle& blockStyle, const bool paragraphIndentEnabled, const int spaceWidth) {
  if (!isIndentAlignment(blockStyle.alignment)) {
    return 0;
  }

  const int cssIndent = positiveTextIndentPx(blockStyle);
  if (cssIndent > 0) {
    return cssIndent;
  }

  if (!paragraphIndentEnabled) {
    return 0;
  }

  // Simple 2-em-ish fallback based on the active font's space width.
  return std::max(0, spaceWidth * 2);
}

// Helper function to split a UTF-8 string into individual characters.
std::vector<std::string> splitUtf8Chars(const std::string& str) {
  std::vector<std::string> chars;
  const char* p = str.c_str();
  while (*p) {
    int charLen = 1;
    const unsigned char c = static_cast<unsigned char>(*p);
    if ((c & 0xF8) == 0xF0) {
      charLen = 4;
    } else if ((c & 0xF0) == 0xE0) {
      charLen = 3;
    } else if ((c & 0xE0) == 0xC0) {
      charLen = 2;
    }
    chars.push_back(std::string(p, charLen));
    p += charLen;
  }
  return chars;
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

// Character-wrap mode: greedy line filling with justified alignment (1.0x-1.5x spacing).
// If spacing would exceed 1.5x, split words at character boundaries to fill the line.
void ParsedText::layoutCharacterWrap(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                     const int spaceWidth,
                                     const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                     const bool includeLastLine) {
  const int pageWidth = viewportWidth;
  const int minSpacing = spaceWidth;
  const int maxSpacing = spaceWidth + (spaceWidth / 2);  // 1.5x
  const int firstLineIndent = effectiveFirstLineIndentPx(blockStyle, paragraphIndent != 0, spaceWidth);

  bool isFirstLine = true;

  while (!words.empty()) {
    const int lineIndent = isFirstLine ? firstLineIndent : 0;
    const int effectivePageWidth = std::max(1, pageWidth - lineIndent);

    std::vector<std::string> lineWordsVec;
    std::vector<int> lineWordWidths;
    std::vector<EpdFontFamily::Style> lineWordStylesVec;

    // Phase 1: Greedily collect words/characters to fill the line.
    int totalWordWidth = 0;

    while (!words.empty()) {
      const std::string& word = words.front();
      const EpdFontFamily::Style wordStyle = wordStyles.front();
      const int wordWidth = measureWordWidth(renderer, fontId, word, wordStyle);

      // Calculate what spacing would be if we add this word.
      const int newTotalWidth = totalWordWidth + wordWidth;
      const int newGapCount = static_cast<int>(lineWordsVec.size());
      const int newSpareSpace = effectivePageWidth - newTotalWidth;
      const int newSpacing = (newGapCount > 0) ? (newSpareSpace / newGapCount) : maxSpacing + 1;

      if (lineWordsVec.empty()) {
        if (wordWidth <= effectivePageWidth) {
          lineWordsVec.push_back(word);
          lineWordWidths.push_back(wordWidth);
          lineWordStylesVec.push_back(wordStyle);
          totalWordWidth = wordWidth;
          words.erase(words.begin());
          wordStyles.erase(wordStyles.begin());
        } else {
          auto chars = splitUtf8Chars(word);
          std::string partial;
          size_t charsFit = 0;

          for (size_t i = 0; i < chars.size(); ++i) {
            std::string test = partial + chars[i];
            const int testWidth = measureWordWidth(renderer, fontId, test, wordStyle);
            if (testWidth > effectivePageWidth) break;
            partial = test;
            charsFit = i + 1;
          }

          if (charsFit == 0) {
            charsFit = 1;
            partial = chars[0];
          }

          const int partialWidth = measureWordWidth(renderer, fontId, partial, wordStyle);
          lineWordsVec.push_back(partial);
          lineWordWidths.push_back(partialWidth);
          lineWordStylesVec.push_back(wordStyle);
          totalWordWidth = partialWidth;

          if (charsFit < chars.size()) {
            std::string remainder;
            for (size_t i = charsFit; i < chars.size(); ++i) {
              remainder += chars[i];
            }
            words.front() = remainder;
          } else {
            words.erase(words.begin());
            wordStyles.erase(wordStyles.begin());
          }
        }
      } else if (newSpacing >= minSpacing) {
        lineWordsVec.push_back(word);
        lineWordWidths.push_back(wordWidth);
        lineWordStylesVec.push_back(wordStyle);
        totalWordWidth = newTotalWidth;
        words.erase(words.begin());
        wordStyles.erase(wordStyles.begin());

        if (newSpacing <= maxSpacing) {
          continue;
        }
      } else {
        const int currentGapCount = static_cast<int>(lineWordsVec.size());
        const int maxPartialWidth = effectivePageWidth - totalWordWidth - currentGapCount * minSpacing;

        if (maxPartialWidth > 0) {
          auto chars = splitUtf8Chars(word);
          std::string partial;
          size_t charsFit = 0;

          for (size_t i = 0; i < chars.size(); ++i) {
            std::string test = partial + chars[i];
            const int testWidth = measureWordWidth(renderer, fontId, test, wordStyle);
            if (testWidth > maxPartialWidth) break;
            partial = test;
            charsFit = i + 1;
          }

          if (charsFit > 0) {
            const int partialWidth = measureWordWidth(renderer, fontId, partial, wordStyle);
            lineWordsVec.push_back(partial);
            lineWordWidths.push_back(partialWidth);
            lineWordStylesVec.push_back(wordStyle);
            totalWordWidth += partialWidth;

            if (charsFit < chars.size()) {
              std::string remainder;
              for (size_t i = charsFit; i < chars.size(); ++i) {
                remainder += chars[i];
              }
              words.front() = remainder;
            } else {
              words.erase(words.begin());
              wordStyles.erase(wordStyles.begin());
            }
          }
        }
        break;
      }
    }

    // Phase 2: If spacing is too large, try to consume a few characters from the next word.
    while (!words.empty() && !lineWordsVec.empty()) {
      const int gapCount = static_cast<int>(lineWordsVec.size());
      const int spareSpace = effectivePageWidth - totalWordWidth;
      const int spacing = (gapCount > 0) ? (spareSpace / gapCount) : 0;

      if (spacing <= maxSpacing) break;

      const std::string& nextWord = words.front();
      const EpdFontFamily::Style nextStyle = wordStyles.front();
      auto chars = splitUtf8Chars(nextWord);

      const int minPartialWidth = effectivePageWidth - totalWordWidth - gapCount * maxSpacing;
      const int maxPartialWidth = effectivePageWidth - totalWordWidth - gapCount * minSpacing;

      if (maxPartialWidth <= 0) break;

      std::string partial;
      int partialWidth = 0;
      size_t charsFit = 0;

      for (size_t i = 0; i < chars.size(); ++i) {
        std::string test = partial + chars[i];
        const int testWidth = measureWordWidth(renderer, fontId, test, nextStyle);
        if (testWidth > maxPartialWidth) break;
        partial = test;
        partialWidth = testWidth;
        charsFit = i + 1;
      }

      if (charsFit == 0 || partialWidth < minPartialWidth) {
        break;
      }

      lineWordsVec.push_back(partial);
      lineWordWidths.push_back(partialWidth);
      lineWordStylesVec.push_back(nextStyle);
      totalWordWidth += partialWidth;

      if (charsFit < chars.size()) {
        std::string remainder;
        for (size_t i = charsFit; i < chars.size(); ++i) {
          remainder += chars[i];
        }
        words.front() = remainder;
      } else {
        words.erase(words.begin());
        wordStyles.erase(wordStyles.begin());
      }
    }

    // Phase 3: Calculate final positions.
    const bool isLastLine = words.empty();
    const int gapCount = static_cast<int>(lineWordsVec.size()) - 1;
    const int spareSpace = effectivePageWidth - totalWordWidth;

    std::vector<std::string> lineWords;
    std::vector<int16_t> lineXPos;
    std::vector<EpdFontFamily::Style> lineWordStyles;

    int xpos = lineIndent;

    if (isLastLine || gapCount <= 0) {
      for (size_t i = 0; i < lineWordsVec.size(); ++i) {
        lineXPos.push_back(static_cast<int16_t>(xpos));
        lineWords.push_back(lineWordsVec[i]);
        lineWordStyles.push_back(lineWordStylesVec[i]);
        xpos += lineWordWidths[i] + minSpacing;
      }
    } else {
      const int baseSpacing = spareSpace / gapCount;
      const int extraPixels = spareSpace % gapCount;

      for (size_t i = 0; i < lineWordsVec.size(); ++i) {
        lineXPos.push_back(static_cast<int16_t>(xpos));
        lineWords.push_back(lineWordsVec[i]);
        lineWordStyles.push_back(lineWordStylesVec[i]);

        if (i < lineWordsVec.size() - 1) {
          const int gap = baseSpacing + (static_cast<int>(i) < extraPixels ? 1 : 0);
          xpos += lineWordWidths[i] + gap;
        }
      }
    }

    if (!lineWords.empty() && (!isLastLine || includeLastLine)) {
      BlockStyle lineBlockStyle = blockStyle;
      lineBlockStyle.alignment = isLastLine ? CssTextAlign::Left : CssTextAlign::Justify;
      processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles),
                                              lineBlockStyle));
    }

    isFirstLine = false;
  }
}

// Consumes data to minimize memory usage.
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Paragraph indent is handled as a coordinate offset, not by mutating the text.
  applyParagraphIndent();

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId);

  if (characterWrap && blockStyle.alignment == CssTextAlign::Justify) {
    layoutCharacterWrap(renderer, fontId, viewportWidth, spaceWidth, processLine, includeLastLine);
    return;
  }

  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, wordContinues);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, wordContinues);
  }

  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, wordContinues, lineBreakIndices, processLine);
  }

  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent = effectiveFirstLineIndentPx(blockStyle, paragraphIndent != 0, spaceWidth);

  for (size_t i = 0; i < wordWidths.size(); ++i) {
    const int effectiveWidth = i == 0 ? std::max(1, pageWidth - firstLineIndent) : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();
  std::vector<int> dp(totalWordCount);
  std::vector<size_t> ans(totalWordCount);

  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = static_cast<int>(totalWordCount) - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    const int effectivePageWidth = i == 0 ? std::max(1, pageWidth - firstLineIndent) : pageWidth;

    for (size_t j = static_cast<size_t>(i); j < totalWordCount; ++j) {
      const int gap = j > static_cast<size_t>(i) && !continuesVec[j] ? spaceWidth : 0;
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];
        cost = cost_ll > MAX_COST ? MAX_COST : static_cast<int>(cost_ll);
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;
      }
    }

    if (dp[i] == MAX_COST) {
      ans[i] = static_cast<size_t>(i);
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;
    if (nextBreakIndex <= currentWordIndex) {
      nextBreakIndex = currentWordIndex + 1;
    }
    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent() {
  // Intentionally empty.
  // Stable indent is handled by X offsets / effective line width, not by inserting visible characters.
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, const int spaceWidth,
                                                            std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  const int firstLineIndent = effectiveFirstLineIndentPx(blockStyle, paragraphIndent != 0, spaceWidth);

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    const int effectivePageWidth = isFirstLine ? std::max(1, pageWidth - firstLineIndent) : pageWidth;

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord || continuesVec[currentIndex] ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    return false;
  }

  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine ? effectiveFirstLineIndentPx(blockStyle, paragraphIndent != 0, spaceWidth) : 0;

  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; ++wordIdx) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      ++actualGapCount;
    }
  }

  const int effectivePageWidth = std::max(1, pageWidth - firstLineIndent);
  const int spareSpace = effectivePageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1) {
    spacing = spareSpace / static_cast<int>(actualGapCount);
  }

  int16_t xpos = static_cast<int16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = static_cast<int16_t>(spareSpace - static_cast<int>(actualGapCount) * spaceWidth);
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = static_cast<int16_t>((spareSpace - static_cast<int>(actualGapCount) * spaceWidth) / 2);
  }

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; ++wordIdx) {
    const uint16_t currentWordWidth = wordWidths[lastBreakAt + wordIdx];
    lineXPos.push_back(xpos);

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    xpos = static_cast<int16_t>(xpos + currentWordWidth + (nextIsContinuation ? 0 : spacing));
  }

  std::vector<std::string> lineWords(std::make_move_iterator(words.begin() + lastBreakAt),
                                     std::make_move_iterator(words.begin() + lineBreak));
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}
