package uni.dosconio.sample_002
// @dosconio 20240419 002 - expr 1

fun main() {
    var number = 99;
    if (number in 10..59) // [10 ~ 59]
        println("D")
    else if (number in 0..9) { // brace style
        println("X")
    }
    else if (number in 60 .. 100)
        println("A")
    else
        println("!")

    val info = when(3) {
        1 -> "One"
        2 -> "Two"
        else -> {
            println("Too many!") // void, whose print-msg is "kotlin.Unit"
        }
    }// info is in type 'Any' for un-united types, while 'Object' in Java
    println(info)
}