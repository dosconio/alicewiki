
#include "system.cs"

#include <stdint.h>
#define _HIS_TIME_H
#include <c/datime.h>

namespace Program
{
	class Program 
	{
		public static void Main() 
		{
			
			Int32 i = 0b10100101;// 165
			Double d = 1.23;
			Boolean b = true;
			Char kanj = *(uint16*)"дв";// ANSI GBK
			Console.WriteLine();
			Console.WriteLine(string("hello ") + i.ToString());
			Console.WriteLine("%+d %d", 12, 12);
			if (*&b) Console.WriteLine("10 * 1.23 == %lf", 10 * *&d);
			Console.WriteLine("%[2c]", *&kanj);
			i = Console.OutFormat("Hello, %s %s.\n", "Happy", "World");
			*&i -= Console.CountFormat("Hello, %s %s.\n", "Happy", "World");
			
			Console.OutFormat("%d\n", i);
			String str;
			str.Format("abc%de %lf %lf\n", 1, 1.2, 3.04);
			Console.OutFormat(str.reference());

			//Console.OutFormat("%d\n", herspan(2025, 1, 1) - herspan(2025, 1, 1));
			Console.OutFormat("%d\n", getYearWeekNumber(2025,12,31));
		}
	};
} ENDF