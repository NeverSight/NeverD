package fixture
private inline fun transfer(p:Provider,i:Int):Any? {
 val marker=Defaults.witness
 val value=p.read(i)
 val mapper=p.mapper(i)
 return if(value==null || value is Boolean)marker else mapper.apply(marker)
}
fun make(p:Provider):Result = Result(transfer(p,0),
 transfer(p,1),
 transfer(p,2),
 transfer(p,3),
 transfer(p,4),
 transfer(p,5),
 transfer(p,6),
 transfer(p,7),
 transfer(p,8),
 transfer(p,9),
 transfer(p,10),
 transfer(p,11),
 transfer(p,12),
 transfer(p,13),
 transfer(p,14),
 transfer(p,15),
 transfer(p,16),
 transfer(p,17),
 transfer(p,18),
 transfer(p,19),
 transfer(p,20),
 transfer(p,21),
 transfer(p,22),
 transfer(p,23))
