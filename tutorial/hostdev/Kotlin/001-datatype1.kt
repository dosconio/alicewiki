package uni.dosconio.sample_001 // optional line
// @dosconio 20240419 Datatype (1)

const val pi = 3.1415926535 // `const` can modify global but local object(value). Evaluate it when compiling.

fun main() {
    // var means variable/mutable
    var a = 1 // auto judge "Int" type/class
    // val means value/constant
    val s : String = "Ciallo Unisym~"
    /* [Data Class]
     * - String
     * - Char
     * - Boolean {true, false}
     * - Int
     * - Double
     * - List
     * - Set
     * - Map
     */
    println(s)

}