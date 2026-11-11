package fixture
class Receipt(@JvmField val token:Any?, @JvmField val stamp:Int):Api<Char> {
    override fun contains(value:Char):Boolean = value.code == stamp
}
