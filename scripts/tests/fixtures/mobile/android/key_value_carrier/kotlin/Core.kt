package fixture
fun <K,V> lookup(map: Map<K,V>,key: K): V = Values.pick(map,key)
fun read(map: Map<Any?,Int>,key: Any?): Int = Values.pick(map,key)
fun <K,V> direct(map: Map<K,V>,key: K): V = map.getValue(key)
fun directRead(map: Map<Any?,Int>,key: Any?): Int = map.getValue(key)
