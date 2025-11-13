// C17 @dosconio GBK

#include <host.h>

// 18位中国人民共和国居民身份证验证，通过返回1
bool Veri_CnID18(const char* s)
{
	// HARUNO change from WEB
	char powr[18] = { 7,9,10,5,8,4,2,1,6,3,7,9,10,5,8,4,2,1 };// last '1' can omitted.
	size_t sum = 0;
	char c;// can omitted to slow the effective.
	for (size_t i = 0; i < 17; i++)
	{
		c = s[i];
		if ('0' <= c && c <= '9')
			sum += (size_t)powr[i] * (c - '0');
		else return 0;
	}
	c = s[17];
	if (c == 'x' || c == 'X')
		c = '0' + 10;
	c -= 0x30;// '0'
	if (c < 0 || c > 11) return 0;
	sum += (size_t)powr[17] * c;
	return (sum % 11 == 1);
}