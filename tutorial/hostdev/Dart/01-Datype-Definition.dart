main() {
  //[Define Variables]
  var str1 = 'Nihao'; // [1] automatically detects the type of the variable
  print(str1); // identidier: alnum, `_` and `$`

  String str2 = 'Hello'; // [2] explicitly specify the type of the variable
  print(str2);
  int num2 = 12;
  print(num2);

  // String str3; // [4] declare a variable without assigning a value
  // print(str3); // invalid : Output: null

  //[Define Constant]
  const PI = 3.1416;
  final e = 2.71828;
  print(PI * e);
}
