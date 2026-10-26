# 词书 TSV 格式

Egga 固件当前支持从 `assets` 分区加载 UTF-8 编码的 TSV 词书文件：

- `/assets/decks/current.tsv`
- `/assets/decks/current.txt`

## 推荐格式

现在推荐的详细格式是 5 列：

```text
word<TAB>phonetic<TAB>meaning<TAB>example<TAB>example_translation
```

对应含义：

1. `word`：单词
2. `phonetic`：音标或发音注释
3. `meaning`：词性 + 翻译
4. `example`：英文例句
5. `example_translation`：例句翻译

元数据行可选，格式为 `# key=value`：

```text
# deck_id=level4luan-2
# name=专四核心词汇
# language=en-zh
# version=1
flutter	/ˈflʌtə(r)/	v. 飘动；鼓翼；烦扰 | n. 摆动；鼓翼；烦扰	Dead leaves fluttered slowly to the ground.	枯叶缓缓飘落到地上。
prawn	/prɔːn/	n. 对虾，明虾	a prawn sandwich	明虾三明治
```

## 兼容格式

固件也兼容更紧凑的旧格式：

- 2 列：`word<TAB>meaning`
- 3 列：`word<TAB>phonetic<TAB>meaning`
- 4 列：`word<TAB>phonetic<TAB>meaning<TAB>example`
- 5 列：`word<TAB>phonetic<TAB>meaning<TAB>example<TAB>example_translation`

如果某些字段为空，仍然可以保留列位置，例如：

```text
abandon	/əˈbændən/	v. 放弃；抛弃		
```

## 显示规则

- 正面显示：单词、音标
- 背面显示：词性+翻译、例句、例句翻译
- 若没有例句翻译，背面只显示例句
- 若没有音标，正面只显示单词

## 转换脚本

仓库里的离线转换脚本：

```bash
python tools/convert_external_deck.py \
  --source ../GRE_2.json \
  --output ../assets/decks/current.tsv \
  --deck-id gre-2 \
  --name "GRE 2"
```

默认输出 5 列详细格式。

如需更小的文件体积，也可以输出紧凑格式：

```bash
python tools/convert_external_deck.py --source ../GRE_2.json --format compact
python tools/convert_external_deck.py --source ../GRE_2.json --format compact-phonetic
```
