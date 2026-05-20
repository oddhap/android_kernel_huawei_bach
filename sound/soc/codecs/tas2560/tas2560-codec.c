/*
 * TAS2560 codec driver for BAH-W09.
 *
 * The known-good speaker setup is the minimal I2S register sequence below.
 * The full 3.18 boost/HPF/limiter tables were tested and caused worse static
 * or silence on this 4-speaker layout.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <sound/soc.h>
#include "tas2560.h"

#define TAS2560_LOAD_REG		TAS2560_REG(0, 0, 9)
#define TAS2560_SR_CTRL1_REG		TAS2560_REG(0, 0, 8)
#define TAS2560_SR_CTRL2_REG		TAS2560_REG(0, 0, 13)
#define TAS2560_SR_CTRL3_REG		TAS2560_REG(0, 0, 14)
#define TAS2560_DAI_FMT_REG		TAS2560_REG(0, 0, 20)
#define TAS2560_ASI_CHANNEL_REG		TAS2560_REG(0, 0, 21)
#define TAS2560_ASI_OFFSET_1_REG	TAS2560_REG(0, 0, 22)
#define TAS2560_CLK_ERR_1_REG		TAS2560_REG(0, 0, 33)
#define TAS2560_CLK_ERR_2_REG		TAS2560_REG(0, 0, 80)

#define TAS2560_CODEC_NAME		"tas2560-smartpa"
#define TAS2560_PRI_DAI			0
#define TAS2560_SEC_DAI			1

#define TIAUDIO_CMD_REG_WRITE		1
#define TIAUDIO_CMD_REG_READ		2
#define TIAUDIO_CMD_DEBUG_ON		3
#define TIAUDIO_CMD_SAMPLERATE		8
#define TIAUDIO_CMD_BITRATE		9
#define TIAUDIO_CMD_DACVOLUME		10
#define TIAUDIO_CMD_SPEAKER		11

#define REG_END				0xFFFFFFFF

static int shared_reset_gpio = -1;
static DEFINE_MUTEX(tas2560_devices_lock);
static struct tas2560_priv *tas2560_devices[4];
static DEFINE_MUTEX(tas2560_misc_lock);
static unsigned int tas2560_misc_current_reg;
static unsigned int tas2560_misc_last_cmd;
static bool tas2560_misc_debug;
static bool tas2560_misc_registered;

static const struct regmap_config tas2560_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
};

static int tas2560_raw_write(struct i2c_client *client,
			     unsigned int addr, unsigned int val)
{
	unsigned int page = TAS2560_PAGE_ID(addr);
	unsigned int reg = TAS2560_PAGE_REG(addr);
	u8 buf[2];
	int ret;

	buf[0] = TAS2560_PAGECTL_REG;
	buf[1] = page;
	ret = i2c_master_send(client, buf, 2);
	if (ret < 0) return ret;

	buf[0] = reg;
	buf[1] = val;
	ret = i2c_master_send(client, buf, 2);
	if (ret < 0) return ret;

	return 0;
}

static void tas2560_load_data(struct i2c_client *client, unsigned int *pData)
{
	unsigned int nRegister, nLength = 0, nLoop, i;
	unsigned int *nData;

	if (!pData)
		return;

	do {
		nRegister = pData[nLength];
		nLoop = pData[nLength + 1];
		nData = &pData[nLength + 2];

		if (nRegister == TAS2560_MDELAY) {
			mdelay(nData[0]);
		} else if (nRegister != REG_END) {
			for (i = 0; i < nLoop; i++)
				tas2560_raw_write(client, nRegister + i, nData[i]);
		}
		nLength = nLength + 2 + pData[nLength + 1];
	} while (nRegister != REG_END);
}

static int tas2560_select_book_page(struct i2c_client *client,
				    unsigned int book, unsigned int page)
{
	u8 buf[2];
	int ret;

	buf[0] = TAS2560_PAGECTL_REG;
	buf[1] = 0;
	ret = i2c_master_send(client, buf, 2);
	if (ret < 0)
		return ret;

	buf[0] = TAS2560_BOOKCTL_REG;
	buf[1] = book;
	ret = i2c_master_send(client, buf, 2);
	if (ret < 0)
		return ret;

	buf[0] = TAS2560_PAGECTL_REG;
	buf[1] = page;
	ret = i2c_master_send(client, buf, 2);
	if (ret < 0)
		return ret;

	return 0;
}

static int tas2560_misc_read_reg(struct tas2560_priv *tas,
				 unsigned int reg, unsigned int *val)
{
	struct i2c_client *client = tas->client;
	u8 addr = TAS2560_PAGE_REG(reg);
	u8 value = 0;
	int ret;

	ret = tas2560_select_book_page(client, TAS2560_BOOK_ID(reg),
				       TAS2560_PAGE_ID(reg));
	if (ret < 0)
		goto out;

	ret = i2c_master_send(client, &addr, 1);
	if (ret < 0)
		goto out;

	ret = i2c_master_recv(client, &value, 1);
	if (ret < 0)
		goto out;

	*val = value;
	ret = 0;

out:
	tas2560_select_book_page(client, 0, 0);
	return ret;
}

static int tas2560_misc_write_reg(struct tas2560_priv *tas,
				  unsigned int reg, unsigned int val)
{
	struct i2c_client *client = tas->client;
	u8 buf[2];
	int ret;

	ret = tas2560_select_book_page(client, TAS2560_BOOK_ID(reg),
				       TAS2560_PAGE_ID(reg));
	if (ret < 0)
		goto out;

	buf[0] = TAS2560_PAGE_REG(reg);
	buf[1] = val;
	ret = i2c_master_send(client, buf, 2);
	if (ret < 0)
		goto out;

	ret = 0;

out:
	tas2560_select_book_page(client, 0, 0);
	return ret;
}

static int tas2560_device_index(unsigned int addr)
{
	switch (addr) {
	case 0x4c:
		return 0;
	case 0x4e:
		return 1;
	case 0x4d:
		return 2;
	case 0x4f:
		return 3;
	default:
		return -EINVAL;
	}
}

static void tas2560_delayed_unmute(struct work_struct *work)
{
	struct tas2560_priv *tas =
		container_of(to_delayed_work(work), struct tas2560_priv,
			     unmute_work);
	unsigned int val = 0;

	mutex_lock(&tas->dev_lock);
	if (tas->enabled && tas->powered) {
		tas2560_raw_write(tas->client, TAS2560_MUTE_REG, 0x40);
		regmap_read(tas->regmap, TAS2560_PWR_REG, &val);
		dev_info(tas->dev, "TAS2560 unmute addr=0x%02x power=0x%02x\n",
			 tas->client->addr, val);
	}
	mutex_unlock(&tas->dev_lock);
}

/* ---- Known-good minimal init data ---- */

static unsigned int tas2560_startup_data[] = {
	TAS2560_CLK_SEL,		0x01, 0x01,
	TAS2560_SET_FREQ,		0x01, 0x10,
	TAS2560_MUTE_REG,		0x01, 0x41,
	TAS2560_MDELAY,			0x01, 0x10,
	REG_END, REG_END
};

static unsigned int tas2560_48khz_data[] = {
	TAS2560_SR_CTRL1_REG,		0x01, 0x01,
	TAS2560_SR_CTRL2_REG,		0x01, 0x08,
	TAS2560_SR_CTRL3_REG,		0x01, 0x10,
	REG_END, REG_END
};

static void tas2560_program_device(struct tas2560_priv *tas)
{
	struct i2c_client *client = tas->client;

	tas2560_load_data(client, tas2560_startup_data);
	tas2560_load_data(client, tas2560_48khz_data);

	tas2560_raw_write(client, TAS2560_DR_BOOST_REG_1, 0x0c);
	tas2560_raw_write(client, TAS2560_DR_BOOST_REG_2, 0x33);
	tas2560_raw_write(client, TAS2560_DEV_MODE_REG, 0x02);
	tas2560_raw_write(client, TAS2560_LOAD_REG, 0x83);
	tas2560_raw_write(client, TAS2560_ASI_CFG_1, TAS2560_TRISTATE);
	tas2560_raw_write(client, TAS2560_DAI_FMT_REG, TAS2560_DATAFORMAT_I2S);

	switch (client->addr) {
	case 0x4c:
	case 0x4d:
		tas2560_raw_write(client, TAS2560_ASI_CHANNEL_REG,
				  TAS2560_ASI_CHANNEL_LEFT);
		tas2560_raw_write(client, TAS2560_ASI_OFFSET_1_REG, 1);
		break;
	case 0x4e:
	case 0x4f:
		tas2560_raw_write(client, TAS2560_ASI_CHANNEL_REG,
				  TAS2560_ASI_CHANNEL_RIGHT);
		tas2560_raw_write(client, TAS2560_ASI_OFFSET_1_REG, 33);
		break;
	}

	tas2560_raw_write(client, TAS2560_CLK_ERR_1_REG, 0x0b);
	tas2560_raw_write(client, TAS2560_CLK_ERR_2_REG, 0x21);
}

static void tas2560_set_power(struct tas2560_priv *tas, bool on)
{
	unsigned int val = 0;

	if (!on)
		cancel_delayed_work_sync(&tas->unmute_work);

	mutex_lock(&tas->dev_lock);
	if (on) {
		tas2560_program_device(tas);
		tas2560_raw_write(tas->client, TAS2560_MUTE_REG, 0x41);
		tas->powered = true;
		schedule_delayed_work(&tas->unmute_work,
				      msecs_to_jiffies(250));
	} else {
		tas2560_raw_write(tas->client, TAS2560_CLK_ERR_1_REG, 0x00);
		tas2560_raw_write(tas->client, TAS2560_MUTE_REG, 0x41);
		tas2560_raw_write(tas->client, TAS2560_MUTE_REG, 0x01);
		msleep(30);
		tas->powered = false;
	}

	regmap_read(tas->regmap, TAS2560_PWR_REG, &val);
	dev_info(tas->dev, "TAS2560 %s addr=0x%02x power=0x%02x\n",
		 on ? "enable" : "disable", tas->client->addr, val);
	mutex_unlock(&tas->dev_lock);
}

static int tas2560_update_bits_locked(struct tas2560_priv *tas,
				      unsigned int reg, unsigned int mask,
				      unsigned int val)
{
	unsigned int old = 0;
	int ret;

	ret = tas2560_misc_read_reg(tas, reg, &old);
	if (ret < 0)
		return ret;

	old &= ~mask;
	old |= val & mask;

	return tas2560_misc_write_reg(tas, reg, old);
}

static int tas2560_set_bit_rate_locked(struct tas2560_priv *tas,
				       unsigned int bit_rate)
{
	unsigned int val;

	switch (bit_rate) {
	case 16:
		val = 0;
		break;
	case 20:
		val = 1;
		break;
	case 24:
		val = 2;
		break;
	case 32:
		val = 3;
		break;
	default:
		return -EINVAL;
	}

	tas->mnBitRate = bit_rate;
	return tas2560_update_bits_locked(tas, TAS2560_DAI_FMT,
					  TAS2560_DAI_WORDLEN_MASK, val);
}

static int tas2560_get_bit_rate_locked(struct tas2560_priv *tas)
{
	unsigned int val = 0;
	int ret;

	ret = tas2560_misc_read_reg(tas, TAS2560_DAI_FMT, &val);
	if (ret < 0)
		return ret;

	switch (val & TAS2560_DAI_WORDLEN_MASK) {
	case 0:
		return 16;
	case 1:
		return 20;
	case 2:
		return 24;
	case 3:
		return 32;
	default:
		return -EINVAL;
	}
}

static int tas2560_set_sample_rate_locked(struct tas2560_priv *tas,
					  unsigned int rate)
{
	switch (rate) {
	case 48000:
		tas2560_load_data(tas->client, tas2560_48khz_data);
		break;
	case 44100:
		tas2560_raw_write(tas->client, TAS2560_SR_CTRL1_REG, 0x11);
		break;
	default:
		return -EINVAL;
	}

	tas->mnSamplingRate = rate;
	return 0;
}

static int tas2560_set_volume_locked(struct tas2560_priv *tas,
				     unsigned int volume)
{
	return tas2560_update_bits_locked(tas, TAS2560_SPK_CTRL_REG, 0x0f,
					  volume & 0x0f);
}

static int tas2560_get_volume_locked(struct tas2560_priv *tas)
{
	unsigned int val = 0;
	int ret;

	ret = tas2560_misc_read_reg(tas, TAS2560_SPK_CTRL_REG, &val);
	if (ret < 0)
		return ret;

	return val & 0x0f;
}

static struct tas2560_priv *tas2560_find_device(int index)
{
	struct tas2560_priv *tas = NULL;

	if (index < 0 || index >= (int)ARRAY_SIZE(tas2560_devices))
		return NULL;

	mutex_lock(&tas2560_devices_lock);
	tas = tas2560_devices[index];
	mutex_unlock(&tas2560_devices_lock);

	return tas;
}

int tas2560_ext_enable_get(unsigned int index)
{
	struct tas2560_priv *tas = tas2560_find_device(index);

	return tas ? tas->enabled : 0;
}

int tas2560_ext_enable_set(unsigned int index, bool enable)
{
	struct tas2560_priv *tas = tas2560_find_device(index);

	if (!tas)
		return -ENODEV;

	if (tas->enabled == enable) {
		if (enable && !tas->powered) {
			tas2560_set_power(tas, true);
			return 1;
		}
		return 0;
	}

	tas->enabled = enable;
	tas2560_set_power(tas, enable);

	return 1;
}

static void tas2560_for_each_device_locked(void (*fn)(struct tas2560_priv *,
						      unsigned int),
					  unsigned int arg)
{
	struct tas2560_priv *tas;
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(tas2560_devices); i++) {
		tas = tas2560_find_device(i);
		if (!tas)
			continue;

		mutex_lock(&tas->dev_lock);
		fn(tas, arg);
		mutex_unlock(&tas->dev_lock);
	}
}

static void tas2560_misc_set_sample_rate_one(struct tas2560_priv *tas,
					     unsigned int rate)
{
	tas2560_set_sample_rate_locked(tas, rate);
}

static void tas2560_misc_set_bit_rate_one(struct tas2560_priv *tas,
					  unsigned int bit_rate)
{
	tas2560_set_bit_rate_locked(tas, bit_rate);
}

static void tas2560_misc_set_volume_one(struct tas2560_priv *tas,
					unsigned int volume)
{
	tas2560_set_volume_locked(tas, volume);
}

static void tas2560_misc_write_reg_one(struct tas2560_priv *tas,
				       unsigned int packed)
{
	tas2560_misc_write_reg(tas, tas2560_misc_current_reg, packed);
}

static ssize_t tas2560_misc_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct tas2560_priv *tas = tas2560_find_device(0);
	u8 *kbuf;
	unsigned int val;
	int ret = 0;
	size_t i;

	if (!tas)
		return -ENODEV;
	if (!count)
		return 0;

	kbuf = kzalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&tas2560_misc_lock);
	mutex_lock(&tas->dev_lock);

	switch (tas2560_misc_last_cmd) {
	case TIAUDIO_CMD_REG_READ:
		for (i = 0; i < count; i++) {
			ret = tas2560_misc_read_reg(tas,
						    tas2560_misc_current_reg + i,
						    &val);
			if (ret < 0)
				break;
			kbuf[i] = val;
		}
		break;
	case TIAUDIO_CMD_SAMPLERATE:
		if (count >= 4) {
			val = tas->mnSamplingRate;
			kbuf[0] = val & 0xff;
			kbuf[1] = (val >> 8) & 0xff;
			kbuf[2] = (val >> 16) & 0xff;
			kbuf[3] = (val >> 24) & 0xff;
		}
		break;
	case TIAUDIO_CMD_BITRATE:
		if (count >= 1) {
			ret = tas2560_get_bit_rate_locked(tas);
			if (ret >= 0) {
				kbuf[0] = ret;
				ret = 0;
			}
		}
		break;
	case TIAUDIO_CMD_DACVOLUME:
		if (count >= 1) {
			ret = tas2560_get_volume_locked(tas);
			if (ret >= 0) {
				kbuf[0] = ret;
				ret = 0;
			}
		}
		break;
	default:
		break;
	}

	mutex_unlock(&tas->dev_lock);
	tas2560_misc_last_cmd = 0;
	mutex_unlock(&tas2560_misc_lock);

	if (ret >= 0 && copy_to_user(buf, kbuf, count))
		ret = -EFAULT;

	kfree(kbuf);
	return ret < 0 ? ret : count;
}

static ssize_t tas2560_misc_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	u8 *kbuf;
	unsigned int reg, val;
	size_t i;

	if (!count)
		return 0;

	kbuf = kzalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, buf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	mutex_lock(&tas2560_misc_lock);
	tas2560_misc_last_cmd = kbuf[0];

	switch (tas2560_misc_last_cmd) {
	case TIAUDIO_CMD_REG_WRITE:
		if (count < 6)
			break;

		reg = ((unsigned int)kbuf[1] << 24) |
		      ((unsigned int)kbuf[2] << 16) |
		      ((unsigned int)kbuf[3] << 8) |
		      kbuf[4];
		for (i = 5; i < count; i++) {
			tas2560_misc_current_reg = reg + i - 5;
			tas2560_for_each_device_locked(tas2560_misc_write_reg_one,
						       kbuf[i]);
		}
		tas2560_misc_last_cmd = 0;
		break;
	case TIAUDIO_CMD_REG_READ:
		if (count == 5) {
			tas2560_misc_current_reg =
				((unsigned int)kbuf[1] << 24) |
				((unsigned int)kbuf[2] << 16) |
				((unsigned int)kbuf[3] << 8) |
				kbuf[4];
			if (tas2560_misc_debug)
				pr_info("tas2560: misc read reg=0x%x\n",
					tas2560_misc_current_reg);
		}
		break;
	case TIAUDIO_CMD_DEBUG_ON:
		if (count >= 2)
			tas2560_misc_debug = !!kbuf[1];
		tas2560_misc_last_cmd = 0;
		break;
	case TIAUDIO_CMD_SAMPLERATE:
		if (count >= 5) {
			val = ((unsigned int)kbuf[1] << 24) |
			      ((unsigned int)kbuf[2] << 16) |
			      ((unsigned int)kbuf[3] << 8) |
			      kbuf[4];
			tas2560_for_each_device_locked(
				tas2560_misc_set_sample_rate_one, val);
			tas2560_misc_last_cmd = 0;
		}
		break;
	case TIAUDIO_CMD_BITRATE:
		if (count >= 2) {
			tas2560_for_each_device_locked(
				tas2560_misc_set_bit_rate_one, kbuf[1]);
			tas2560_misc_last_cmd = 0;
		}
		break;
	case TIAUDIO_CMD_DACVOLUME:
		if (count >= 2) {
			tas2560_for_each_device_locked(
				tas2560_misc_set_volume_one, kbuf[1]);
			tas2560_misc_last_cmd = 0;
		}
		break;
	case TIAUDIO_CMD_SPEAKER:
		if (count >= 2) {
			for (i = 0; i < ARRAY_SIZE(tas2560_devices); i++)
				tas2560_ext_enable_set(i, !!kbuf[1]);
			tas2560_misc_last_cmd = 0;
		}
		break;
	default:
		tas2560_misc_last_cmd = 0;
		break;
	}

	mutex_unlock(&tas2560_misc_lock);
	kfree(kbuf);
	return count;
}

static const struct file_operations tas2560_misc_fops = {
	.owner = THIS_MODULE,
	.read = tas2560_misc_read,
	.write = tas2560_misc_write,
	.llseek = no_llseek,
};

static struct miscdevice tas2560_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "tas2560",
	.fops = &tas2560_misc_fops,
};

void tas2560_ext_regdump(void)
{
	struct tas2560_priv *tas;
	unsigned int power = 0, flags1 = 0, flags2 = 0;
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(tas2560_devices); i++) {
		tas = tas2560_find_device(i);
		if (!tas)
			continue;

		mutex_lock(&tas->dev_lock);
		regmap_read(tas->regmap, TAS2560_PWR_REG, &power);
		regmap_read(tas->regmap, TAS2560_FLAGS_1, &flags1);
		regmap_read(tas->regmap, TAS2560_FLAGS_2, &flags2);
		mutex_unlock(&tas->dev_lock);

		dev_info(tas->dev,
			 "TAS2560 regdump idx=%d addr=0x%02x enable=%d power=0x%02x flags=0x%02x/0x%02x\n",
			 i, tas->client->addr, tas->enabled, power, flags1,
			 flags2);
	}
}

/* ---- ASoC codec ---- */

static int tas2560_group_first(struct snd_soc_dai *dai)
{
	return dai && dai->id == TAS2560_SEC_DAI ? 2 : 0;
}

static int tas2560_aif_post_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	int first = 0;
	int i;

	if (w->name && !strncmp(w->name, "ASI2", strlen("ASI2")))
		first = 2;

	for (i = first; i < first + 2; i++) {
		struct tas2560_priv *tas = tas2560_find_device(i);

		if (!tas)
			continue;
		if (event == SND_SOC_DAPM_POST_PMU && !tas->enabled)
			continue;

		dev_info(tas->dev, "TAS2560 %s %s addr=0x%02x\n",
			 w->name ? w->name : "AIF",
			 event == SND_SOC_DAPM_POST_PMU ? "power up" :
			 "power down", tas->client->addr);
		tas2560_set_power(tas, event == SND_SOC_DAPM_POST_PMU);
	}

	return 0;
}

static const struct snd_soc_dapm_widget tas2560_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN_E("ASI1", "ASI1 Playback", 0, SND_SOC_NOPM, 0, 0,
			      tas2560_aif_post_event,
			      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN_E("ASI2", "ASI2 Playback", 1, SND_SOC_NOPM, 0, 0,
			      tas2560_aif_post_event,
			      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC("DAC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUT_DRV("ClassD", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_OUTPUT("OUT"),
};

static const struct snd_soc_dapm_route tas2560_audio_map[] = {
	{"DAC", NULL, "ASI1"},
	{"DAC", NULL, "ASI2"},
	{"ClassD", NULL, "DAC"},
	{"OUT", NULL, "ClassD"},
};

static int tas2560_dai_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	dev_info(dai->dev, "TAS2560 %s startup stream=%d\n", dai->name,
		 substream->stream);
	return 0;
}

static int tas2560_dai_mute(struct snd_soc_dai *dai, int mute)
{
	int first = tas2560_group_first(dai);
	int i;

	for (i = first; i < first + 2; i++) {
		struct tas2560_priv *tas = tas2560_find_device(i);

		if (!tas)
			continue;

		if (mute)
			cancel_delayed_work_sync(&tas->unmute_work);
		else
			cancel_delayed_work(&tas->unmute_work);

		mutex_lock(&tas->dev_lock);
		if (tas->enabled && tas->powered) {
			if (mute) {
				tas2560_raw_write(tas->client, TAS2560_MUTE_REG,
						  0x41);
				dev_info(tas->dev,
					 "TAS2560 dai mute addr=0x%02x\n",
					 tas->client->addr);
			} else {
				schedule_delayed_work(&tas->unmute_work,
						      msecs_to_jiffies(80));
				dev_info(tas->dev,
					 "TAS2560 dai unmute pending addr=0x%02x\n",
					 tas->client->addr);
			}
		}
		mutex_unlock(&tas->dev_lock);
	}

	return 0;
}

static int tas2560_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	dev_info(dai->dev, "TAS2560 %s set_fmt=0x%x\n", dai->name, fmt);
	return 0;
}

static const struct snd_soc_dai_ops tas2560_dai_ops = {
	.startup = tas2560_dai_startup,
	.digital_mute = tas2560_dai_mute,
	.set_fmt = tas2560_dai_set_fmt,
};

static int tas2560_codec_probe(struct snd_soc_component *component)
{
	snd_soc_dapm_ignore_suspend(&component->dapm, "OUT");
	return 0;
}

static void tas2560_codec_remove(struct snd_soc_component *component) {}

static struct snd_soc_component_driver soc_codec_driver_tas2560 = {
	.probe = tas2560_codec_probe,
	.remove = tas2560_codec_remove,
	.dapm_widgets = tas2560_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tas2560_dapm_widgets),
	.dapm_routes = tas2560_audio_map,
	.num_dapm_routes = ARRAY_SIZE(tas2560_audio_map),
};

static struct snd_soc_dai_driver tas2560_dai_driver[] = {
	{
		.name = "tas2560-asi-pri",
		.id = TAS2560_PRI_DAI,
		.playback = {
			.stream_name = "ASI1 Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE,
		},
		.ops = &tas2560_dai_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "tas2560-asi-sec",
		.id = TAS2560_SEC_DAI,
		.playback = {
			.stream_name = "ASI2 Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE,
		},
		.ops = &tas2560_dai_ops,
		.symmetric_rates = 1,
	},
};

static int tas2560_i2c_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct tas2560_priv *tas;
	struct device_node *np = client->dev.of_node;
	int ret;
	unsigned int val;
	int index;

	dev_info(&client->dev, "TAS2560 probe addr=0x%02x\n", client->addr);

	tas = devm_kzalloc(&client->dev, sizeof(*tas), GFP_KERNEL);
	if (!tas) return -ENOMEM;
	tas->dev = &client->dev;
	tas->client = client;
	mutex_init(&tas->dev_lock);
	INIT_DELAYED_WORK(&tas->unmute_work, tas2560_delayed_unmute);
	i2c_set_clientdata(client, tas);
	index = tas2560_device_index(client->addr);

	if (np) {
		tas->mnLeftResetGPIO = of_get_named_gpio(np, "ti,reset-gpio", 0);
		tas->mnLeftIRQGPIO = of_get_named_gpio(np, "ti,irq-gpio", 0);
	}

	if (gpio_is_valid(tas->mnLeftResetGPIO) && shared_reset_gpio < 0) {
		ret = devm_gpio_request_one(&client->dev, tas->mnLeftResetGPIO,
					     GPIOF_OUT_INIT_LOW, "tas2560_reset");
		if (ret) return ret;
		shared_reset_gpio = tas->mnLeftResetGPIO;
		msleep(5);
		gpio_set_value(shared_reset_gpio, 1);
		msleep(5);
	}

	tas->regmap = devm_regmap_init_i2c(client, &tas2560_regmap_config);
	if (IS_ERR(tas->regmap)) return PTR_ERR(tas->regmap);

	regmap_write(tas->regmap, TAS2560_SW_RESET_REG, 0x01);
	msleep(5);

	tas2560_program_device(tas);
	tas2560_set_power(tas, false);
	tas->mnSamplingRate = 48000;
	tas->mnBitRate = 16;

	val = 0;
	regmap_read(tas->regmap, TAS2560_PWR_REG, &val);
	dev_info(&client->dev, "TAS2560 power=0x%02x addr=0x%02x\n", val, client->addr);

	if (index >= 0) {
		mutex_lock(&tas2560_devices_lock);
		tas2560_devices[index] = tas;
		mutex_unlock(&tas2560_devices_lock);
	}

	if (client->addr == 0x4c) {
		dev_set_name(&client->dev, TAS2560_CODEC_NAME);
		ret = devm_snd_soc_register_component(&client->dev,
						      &soc_codec_driver_tas2560,
						      tas2560_dai_driver,
						      ARRAY_SIZE(tas2560_dai_driver));
		if (ret)
			return ret;
		dev_info(&client->dev, "TAS2560 codec registered as %s\n",
			 TAS2560_CODEC_NAME);

		if (!tas2560_misc_registered) {
			ret = misc_register(&tas2560_misc_device);
			if (ret)
				dev_err(&client->dev,
					"TAS2560 misc register failed: %d\n",
					ret);
			else {
				tas2560_misc_registered = true;
				dev_info(&client->dev,
					 "TAS2560 misc registered as /dev/tas2560\n");
			}
		}
	}

	dev_info(&client->dev, "TAS2560 OK addr=0x%02x\n", client->addr);
	return 0;
}

static const struct i2c_device_id tas2560_i2c_id[] = { { "tas2560", 0 }, {} };
MODULE_DEVICE_TABLE(i2c, tas2560_i2c_id);

static const struct of_device_id tas2560_of_match[] = {
	{ .compatible = "ti,tas2560s" },
	{ .compatible = "ti,tas2560" },
	{}
};
MODULE_DEVICE_TABLE(of, tas2560_of_match);

static struct i2c_driver tas2560_i2c_driver = {
	.driver = { .name = "tas2560", .of_match_table = tas2560_of_match },
	.probe = tas2560_i2c_probe,
	.id_table = tas2560_i2c_id,
};
module_i2c_driver(tas2560_i2c_driver);
MODULE_LICENSE("GPL");
