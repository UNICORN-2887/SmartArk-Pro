"""Generate pinyin→Chinese character lookup table for embedded keyboard.
Output: pinyin_table.h — C header with sorted pinyin entries.
Each entry: pinyin string + array of matching characters (sorted by frequency).
"""
import os, sys, re, struct
from collections import OrderedDict

# ── Embedded pinyin data: syllable → characters (by frequency) ──
# Covers ~2500 most common Chinese characters (GB2312 Level 1)
PY_DATA = """
a:啊阿
ai:爱矮挨哎碍癌艾哀蔼
an:安按暗岸案俺鞍黯
ang:昂
ao:奥傲熬澳凹袄
ba:把吧八巴拔坝霸爸扒罢靶
bai:百白败摆伯拜柏呗
ban:半办班版般搬板扮伴瓣颁拌斑扮绊
bang:帮邦榜棒绑傍谤磅
bao:保报包宝暴薄爆抱胞饱剥堡雹
bei:被北备背悲辈杯碑卑贝倍臂呗
ben:本奔笨
beng:蹦泵
bi:比笔必避逼鼻彼壁臂闭币逼碧蔽毕弊鄙毙
bian:边变便遍编辩鞭扁辨贬卞
biao:表标彪膘飙飚
bie:别憋鳖瘪
bin:宾滨彬斌濒
bing:并病兵冰柄丙饼秉
bo:波播剥薄伯博玻驳脖泊卜拨勃帛
bu:不部步布补捕卜埠簿哺
ca:擦
cai:才采菜材财彩裁猜踩蔡
can:参餐残惨蚕灿惭
cang:藏仓苍舱沧
cao:草操曹槽嘈
ce:侧策测册厕
ceng:层曾蹭
cha:查差察茶插叉刹岔诧碴
chai:拆柴差豺钗
chan:产颤缠铲阐搀蝉馋谗潺
chang:长场常唱尝偿昌畅倡敞肠猖
chao:超朝潮嘲吵抄巢炒超
che:车撤扯彻掣澈
chen:称沉晨陈臣尘衬辰衬趁忱
cheng:成城程承称呈诚惩撑澄乘秤橙
chi:吃持迟尺赤池翅齿耻斥炽痴弛驰侈
chong:重冲充崇宠虫
chou:抽愁仇筹丑绸畴酬稠瞅
chu:出处初除楚础储触畜厨锄雏橱矗
chuai:揣
chuan:传穿船串川喘
chuang:创窗床闯
chui:吹垂锤炊捶
chun:春纯唇醇蠢椿
chuo:绰戳
ci:此次词辞刺磁慈瓷雌
cong:从丛匆聪葱囱
cou:凑
cu:促粗醋簇蹴
cuan:窜攒篡蹿
cui:催脆翠崔摧粹璀
cun:存村寸
cuo:错措挫磋撮蹉
da:大打达答搭瘩
dai:大代带待袋戴呆贷逮歹怠殆
dan:但单担弹淡蛋胆旦氮诞丹惮眈
dang:当党档荡挡档
dao:到道导倒刀盗岛稻悼捣蹈祷
de:的地得德
deng:等登灯邓凳蹬瞪
di:地第低底敌抵弟递帝滴堤笛涤狄
dian:点电店典垫殿淀奠惦颠碘甸
diao:调掉雕吊钓刁貂
die:跌叠爹蝶碟谍
ding:定订顶丁盯钉鼎锭叮
diu:丢
dong:动东冬懂洞冻董栋恫
dou:都斗豆抖逗兜陡窦痘
du:都度独读堵渡杜肚督赌毒睹妒
duan:段断短端锻缎
dui:对队堆兑
dun:吨顿盾蹲墩钝炖
duo:多夺朵躲堕舵垛踱
e:恶额鹅俄扼遏鄂厄峨
en:恩
er:而二儿尔耳
fa:发法罚伐阀乏筏
fan:反饭翻犯范番繁凡烦泛返帆藩
fang:方放房防访仿纺芳坊妨舫
fei:非飞费肥废匪菲沸肺
fen:分份纷粉坟奋芬焚愤氛汾
feng:风丰封峰锋疯枫逢冯奉凤缝
fu:服副夫复富福父负附付妇扶浮幅符腐辅伏覆抚芙釜腑赴袱甫
ga:嘎
gai:改该盖概钙溉
gan:感干敢赶甘肝杆竿尴
gang:刚钢港岗纲杠缸肛
gao:高告搞稿糕膏犒
ge:个歌格割隔革阁戈葛鸽搁疙
gei:给
gen:跟根
geng:更耕庚羹
gong:工公共供功攻宫巩拱贡躬汞
gou:构够狗购沟勾苟钩
gu:故顾股鼓骨古孤姑雇谷辜菇箍估固咕
gua:挂瓜刮寡卦呱
guai:怪拐乖
guan:关管官观馆冠惯灌罐棺贯
guang:光广逛
gui:贵归规鬼桂柜龟硅轨跪诡闺
gun:滚棍
guo:过国果裹锅郭
ha:哈
hai:还海害孩
han:汉喊含寒旱憾韩捍翰涵悍焊罕
hang:行航杭
hao:好号毫豪耗浩嚎
he:和合河何核喝贺赫荷褐盒禾鹤
hei:黑嘿
hen:很恨狠痕
heng:横恒衡哼亨
hong:红宏洪虹鸿轰哄弘泓
hou:后候厚侯猴吼
hu:护户呼湖互忽胡虎糊壶乎弧狐唬沪
hua:话花化华划画滑哗桦
huai:坏怀淮徊
huan:还欢环换缓患唤焕幻桓宦涣
huang:黄慌皇荒晃煌惶簧谎恍
hui:会回汇辉恢挥惠灰毁慧晦贿秽徽卉
hun:婚混魂昏浑馄
huo:活或火伙获货霍祸惑豁
ji:几机及记计级极集技际积激继急基既绩纪季击济辑鸡迹吉籍疾祭饥脊肌嫉忌畸姬
jia:家加价假架佳嫁驾夹甲钾嘉
jian:见间建件简坚检渐剑尖肩艰监键健兼减捡鉴剪拣碱歼茧柬俭
jiang:将讲江降奖浆疆匠蒋姜僵桨酱
jiao:教叫交较角觉脚焦胶郊搅骄娇椒礁缴绞
jie:接结节解姐界阶借介戒揭杰截捷竭洁劫睫
jin:进金近今尽紧禁劲仅锦筋斤谨晋浸巾襟
jing:经精景京警竟静敬惊井镜睛竞净境晶径
jiong:窘
jiu:就九久旧酒纠救究舅灸韭
ju:具局据举居剧聚拒距巨惧菊矩俱锯鞠驹
juan:卷捐倦娟绢眷
jue:觉决绝角掘诀爵倔崛
jun:军均俊峻菌君骏竣钧
ka:卡咖
kai:开凯概楷
kan:看刊堪砍勘坎侃
kang:抗康炕扛慷
kao:考靠烤拷犒
ke:可科客课颗刻壳渴克柯呵苛磕棵
ken:肯垦恳啃
keng:坑
kong:空孔控恐
kou:口扣寇叩
ku:苦哭库裤酷窟枯骷
kua:跨夸垮挎
kuai:快块筷会计
kuan:宽款
kuang:况矿狂框旷眶匡筐
kui:亏愧溃葵馈魁窥盔
kun:困昆捆坤
kuo:扩括阔廓
la:拉啦辣腊蜡喇
lai:来赖莱
lan:兰蓝烂拦览栏懒缆篮滥澜
lang:浪朗狼郎廊榔琅
lao:老劳老板牢捞姥佬唠烙
le:了乐勒
lei:类累雷泪蕾垒磊擂
leng:冷愣
li:里力理利立离例历礼粒厉丽璃励梨厘犁黎篱狸痢
lia:俩
lian:连联练脸莲怜帘廉链敛炼
liang:两亮良量凉粮梁辆晾谅粱
liao:了料疗辽聊廖僚寥撩
lie:列烈裂猎劣
lin:林临邻淋琳磷凛鳞
ling:另令领灵零龄岭玲铃凌陵菱伶翎
liu:六流留刘柳溜瘤榴硫琉
long:龙弄隆笼聋拢垄
lou:楼漏露搂陋篓
lu:路陆录露炉卢鹿鲁芦颅庐碌卤赂
lv:旅绿率律虑铝侣履缕吕氯
luan:乱卵峦
lue:略掠
lun:论轮伦沦
luo:落罗络洛逻裸骆萝锣
ma:吗妈马码骂麻玛蚂嘛蟆
mai:买卖迈麦埋脉
man:满慢漫蛮蔓瞒曼馒
mang:忙盲芒茫氓
mao:毛冒猫帽贸矛貌茅茂锚
me:么
mei:没美每妹煤眉梅媒霉枚媚昧
men:门们闷
meng:梦猛蒙盟萌孟锰朦
mi:米密秘迷蜜谜觅泌靡
mian:面棉免眠绵勉缅
miao:秒妙描庙苗瞄渺藐
mie:灭蔑
min:民敏闽
ming:名明命鸣铭冥
miu:谬
mo:末模莫默磨魔摸墨漠沫膜摩寞
mou:某谋
mu:目母木墓幕牧慕姆穆暮募
na:那拿哪纳娜呐钠
nai:乃奶耐奈氖
nan:男难南
nang:囊
nao:脑闹恼
ne:呢
nei:内
nen:嫩
neng:能
ni:你拟尼泥逆妮匿腻溺
nian:年念碾
niang:娘酿
niao:鸟尿
nie:捏聂镍孽
nin:您
ning:宁凝拧柠狞
niu:牛纽扭钮
nong:农弄浓
nu:努怒奴
nv:女
nuan:暖
nuo:诺挪懦糯
o:哦
ou:欧偶殴藕呕
pa:怕爬帕趴琶帕
pai:派排拍牌徘湃
pan:判盘盼叛攀畔潘
pang:旁胖庞
pao:跑炮泡抛袍刨
pei:配陪培佩赔沛裴胚
pen:盆喷
peng:碰朋彭棚蓬膨砰澎篷
pi:批皮匹脾疲辟劈屁僻譬啤琵
pian:片篇偏骗翩
piao:票飘漂瓢
pie:撇
pin:品贫频拼
ping:平评瓶凭萍屏苹
po:破坡泼婆迫颇泊
pu:普铺扑朴谱仆蒲葡瀑匍圃曝
qi:其起七气期奇器企启妻弃汽棋骑旗齐祈歧契栖戚泣漆脐
qia:恰卡掐
qian:前钱千签迁浅潜牵谦乾纤欠歉铅嵌黔谴
qiang:强抢墙枪腔呛锵
qiao:桥悄巧敲俏瞧乔侨撬鞘
qie:切且窃怯
qin:亲秦勤琴侵寝禽擒钦芹沁
qing:请清轻青情晴庆倾氢卿擎氰
qiong:穷琼穹
qiu:求秋球丘邱囚酋蚯
qu:去取区曲趣趋驱屈渠娶瞿龋
quan:全权圈泉劝拳犬券
que:却缺确雀瘸鹊
qun:群裙逡
ran:然染燃冉
rang:让壤嚷
rao:绕扰饶
re:热惹
ren:人认任忍仁韧刃妊
reng:仍扔
ri:日
rong:容荣融绒蓉溶熔冗茸
rou:肉柔揉
ru:如入儒乳辱蠕褥
ruan:软
rui:锐瑞
run:润闰
ruo:若弱
sa:洒撒萨
sai:赛塞腮
san:三散伞
sang:桑丧嗓
sao:扫嫂搔骚
se:色涩瑟
sen:森
seng:僧
sha:沙杀傻纱刹砂鲨煞
shai:晒筛
shan:山善扇闪珊杉擅衫删煽赡
shang:上商伤赏尚裳晌
shao:少绍烧稍勺梢哨邵
she:设社射蛇涉舍奢赦
shei:谁
shen:什深身神审伸甚慎渗申绅呻肾
sheng:生声省升胜剩圣盛绳
shi:是时十事实市世识使师士示石施始势史式失食室适视湿释诗饰尸拾逝侍誓
shou:手受收首守售授寿瘦兽狩
shu:数书树属术输述熟束舒殊叔鼠疏竖薯梳淑枢暑墅漱
shua:刷耍
shuai:率摔甩衰帅
shuan:拴栓
shuang:双爽霜
shui:水谁睡税
shun:顺瞬舜
shuo:说
si:四思死司丝似私寺撕嗣饲肆嘶
song:送松宋诵耸颂讼
sou:搜艘
su:速苏素诉塑肃俗宿粟溯酥
suan:算酸蒜
sui:随虽岁碎遂隋穗
sun:孙损笋
suo:所缩锁索
ta:他她它踏塔塌
tai:太台态抬泰胎汰
tan:谈弹探叹坦摊滩碳贪毯坛谭瘫
tang:堂躺唐糖汤倘塘烫趟棠膛
tao:套逃讨桃淘涛陶萄掏滔
te:特
teng:腾疼藤誊
ti:提题体替梯踢剔剃蹄啼涕屉
tian:天田填甜添
tiao:条调跳挑迢
tie:贴铁帖
ting:听停庭厅挺亭艇廷
tong:同通统痛铜筒童桶桐彤瞳佟
tou:头投透偷
tu:土图突途涂吐兔屠凸秃
tuan:团
tui:推退腿
tun:吞屯
tuo:脱托拖妥拓驼椭唾驮
wa:瓦挖蛙洼娃袜
wai:外歪
wan:完晚万玩湾碗弯挽顽宛婉丸腕
wang:往王望网忘亡旺汪枉
wei:为位未委维围威微卫味危违谓唯尾魏胃慰伪纬萎巍帷苇
wen:文问温闻稳吻纹蚊紊
weng:翁
wo:我握窝卧涡蜗
wu:五无物武务午误舞雾悟屋污乌吴伍巫勿梧侮钨戌
xi:西喜细吸戏系希息习稀席洗惜析熄膝夕悉溪熙锡晰犀嘻媳牺隙犀
xia:下夏瞎吓峡侠暇狭霞匣虾辖
xian:先现线显限险献县鲜闲宪陷掀纤弦咸贤衔舷涎
xiang:想向相象响项香乡箱享详巷祥湘厢橡翔镶
xiao:小笑消效校销晓萧孝肖削潇硝嚣
xie:写些血谢鞋协斜械卸歇携泄邪胁蟹谐懈
xin:心新信辛欣薪芯锌馨
xing:行性星形兴型刑醒姓幸杏邢腥猩
xiong:兄胸雄凶熊匈
xiu:修秀休袖羞朽绣嗅锈
xu:许需续须虚序绪畜蓄叙徐旭婿絮吁
xuan:选宣旋悬玄喧轩绚眩癣
xue:学雪血穴靴薛削
xun:寻训迅讯巡询循勋熏旬驯殉汛
ya:压牙亚呀鸭芽崖哑涯衙雅讶轧
yan:眼烟演言沿严颜延岩研掩盐厌宴炎验燕艳雁咽焰砚阎淹焉衍谚
yang:样阳央养洋扬杨氧仰痒羊鸯漾
yao:要药摇腰咬耀遥姚窑邀谣舀
ye:也业夜叶爷野页液耶
yi:一已意义以艺易衣依移异益遗疑仪亿亦翼译忆椅伊姨溢毅疫谊抑矣蚁逸壹
yin:因音引银印饮阴隐姻吟殷茵淫寅
ying:应影英营迎硬映赢鹰颖盈婴缨荧莹萤
yo:哟
yong:用永拥勇涌泳庸踊咏雍俑
you:有又由友油游右优尤忧邮幽悠犹诱佑柚
yu:于与语鱼雨余预域育遇欲愈寓玉御裕狱誉浴渔吁郁豫淤芋
yuan:远员原元院愿园圆源怨缘冤渊援猿袁辕
yue:越月乐约阅跃悦岳
yun:运云允韵孕匀蕴酝
za:杂砸咋
zai:在再灾载栽宰哉
zan:赞暂咱攒
zang:脏藏葬
zao:早造遭糟澡燥枣躁凿藻
ze:则责择泽
zei:贼
zen:怎
zeng:增赠憎
zha:炸扎渣闸眨乍榨诈札
zhai:摘宅窄债寨斋
zhan:占战站展沾粘斩盏绽辗瞻崭
zhang:长张章掌丈仗障涨帐账彰樟杖
zhao:找照招赵召兆罩朝沼肇
zhe:这着者折哲浙遮蔗蛰辙
zhen:真阵针震镇振珍诊枕斟侦臻贞
zheng:正整政证争征郑挣症蒸睁铮
zhi:之只知直制至指质志支值治职致纸止置智植址秩芝枝植旨滞趾稚挚
zhong:中种重众终钟忠肿仲衷盅
zhou:周州洲粥轴舟皱肘咒昼纣
zhu:主注住猪著助珠筑祝驻逐柱朱竹诸煮烛铸瞩蛛诛贮
zhua:抓爪
zhuai:拽
zhuan:转专砖赚撰篆
zhuang:装壮状庄撞桩妆幢
zhui:追坠缀椎锥
zhun:准
zhuo:桌捉琢卓浊拙酌灼茁
zi:自子字资紫姿滋籽咨渍孜
zong:总宗纵综棕踪
zou:走奏邹揍
zu:组族足阻租祖
zuan:钻
zui:最醉嘴罪
zun:尊遵
zuo:做作坐左座昨佐琢
"""

def parse_pinyin_data():
    """Parse PY_DATA into OrderedDict {pinyin: [characters]}."""
    table = OrderedDict()
    for line in PY_DATA.strip().split("\n"):
        if not line.strip() or not ":" in line:
            continue
        py, chars = line.split(":", 1)
        py = py.strip()
        chars = chars.strip()
        if py and chars:
            table[py] = list(chars)
    return table

def generate_header(table, output_path):
    """Generate C header with pinyin lookup table.
    Format: two arrays — pinyin_index (offsets + strings) + char_table (characters).
    """
    entries = list(table.items())
    total_chars = sum(len(chars) for _, chars in entries)

    header = []
    header.append("// Auto-generated by gen_pinyin_table.py")
    header.append(f"// {len(entries)} pinyin syllables, {total_chars} characters")
    header.append("#pragma once")
    header.append("#include <stdint.h>")
    header.append("")

    # Pinyin string table (null-separated, double-null terminated)
    py_data = bytearray()
    py_offsets = []
    for py, chars in entries:
        py_offsets.append(len(py_data))
        py_data.extend(py.encode('utf-8'))
        py_data.append(0)
    py_data.append(0)  # double-null terminator

    header.append(f"// Pinyin strings ({len(py_data)} bytes)")
    header.append(f"static const uint16_t PY_OFFSETS[{len(entries)}] = {{")
    header.append("    " + ", ".join(str(o) for o in py_offsets))
    header.append("};")
    header.append("")
    header.append(f"static const uint8_t PY_STRINGS[{len(py_data)}] = {{")
    for i in range(0, len(py_data), 16):
        header.append("    " + ", ".join(f"0x{b:02X}" for b in py_data[i:i+16]) + ",")
    header.append("};")
    header.append("")

    # Character table: per-entry, first byte = count, then UTF-8 chars
    char_data = bytearray()
    char_offsets = []
    for py, chars in entries:
        char_offsets.append(len(char_data))
        char_data.append(len(chars))
        for c in chars:
            char_data.extend(c.encode('utf-8'))
            char_data.append(0)

    header.append(f"// Character table ({len(char_data)} bytes)")
    header.append(f"static const uint16_t CHAR_OFFSETS[{len(entries)}] = {{")
    header.append("    " + ", ".join(str(o) for o in char_offsets))
    header.append("};")
    header.append("")
    header.append(f"static const uint8_t CHAR_TABLE[{len(char_data)}] = {{")
    for i in range(0, len(char_data), 16):
        header.append("    " + ", ".join(f"0x{b:02X}" for b in char_data[i:i+16]) + ",")
    header.append("};")
    header.append("")
    header.append(f"#define PY_COUNT {len(entries)}")
    header.append("")

    # Lookup function
    header.append("// Lookup: returns pointer to char_table data for pinyin, or NULL")
    header.append("// Data format: [count(1B)] [char_utf8(NUL)] [char_utf8(NUL)] ...")
    header.append("static inline const uint8_t* py_lookup(const char* pinyin) {")
    header.append("    int lo = 0, hi = PY_COUNT - 1;")
    header.append("    while (lo <= hi) {")
    header.append("        int mid = (lo + hi) / 2;")
    header.append("        const char* s = (const char*)(PY_STRINGS + PY_OFFSETS[mid]);")
    header.append("        int cmp = strcmp(pinyin, s);")
    header.append("        if (cmp == 0) return CHAR_TABLE + CHAR_OFFSETS[mid];")
    header.append("        if (cmp < 0) hi = mid - 1;")
    header.append("        else lo = mid + 1;")
    header.append("    }")
    header.append("    return NULL;")
    header.append("}")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(header) + "\n")

    # Also generate a plain text version for reference
    txt_path = output_path.replace(".h", ".txt")
    with open(txt_path, "w", encoding="utf-8") as f:
        for py, chars in entries:
            f.write(f"{py}: {''.join(chars)}\n")

    sz = os.path.getsize(output_path)
    print(f"[OK] {output_path}")
    print(f"     {len(entries)} syllables, {total_chars} characters, {sz/1024:.0f}KB")
    print(f"     Reference: {txt_path}")

if __name__ == "__main__":
    table = parse_pinyin_data()
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "pinyin_table.h")
    generate_header(table, out)
