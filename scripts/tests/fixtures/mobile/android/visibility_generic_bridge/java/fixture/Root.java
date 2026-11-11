package fixture;
class Root {
    public Object read(Object first,Object second,java.util.List<String[]> values,int index) {
        Trace.hit(1,this,first,second,values,index,null);
        String[] array=values.get(index);
        Trace.hit(2,this,first,second,values,index,array);
        Object result=array[Trace.offset];
        Trace.hit(3,this,first,second,values,index,result);
        return result;
    }
}
