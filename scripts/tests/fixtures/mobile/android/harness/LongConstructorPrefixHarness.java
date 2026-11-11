import fixture.*;
public class LongConstructorPrefixHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned long constructor prefix");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int mask=mode==32?4:(mode%4)<<1;
        int[] widths={Integer.MIN_VALUE,Integer.MAX_VALUE,-1,0};
        int[] heights={Integer.MAX_VALUE,Integer.MIN_VALUE,0,-1};
        int[] failures={5,1,2,3,6,6,4,5};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.partial=null;Trace.imageSeen=null;Trace.offsetSeen=0;Trace.sizeSeen=0;Trace.packed=0;
            int index=((mode/4)+attempt)%4,width=widths[index],height=heights[index];
            Trace.width=width;Trace.height=height;Trace.fail=mode>=16&&mode<32?failures[mode%8]:0;
            Trace.failure=mode<24?new IllegalStateException("owned runtime"):new AssertionError("owned error");
            long offset=0x1234567887654321L^((long)mode<<29)^attempt,size=~offset;
            long seed=0x80000000abcdef01L^(mode+attempt),result=0xfedcba9876543210L^(mode*257L+attempt);
            Trace.offsetValue=seed;Trace.result=result;
            Shape image=mode==32?null:new LongConstructorPrefixProbe();Core value=null;Throwable failure=null;
            try{value=LongConstructorPrefixCaller.make(route,image,offset,size,mask);}catch(Throwable error){failure=error;}
            long events=0;boolean primary=false,seen=false,packedSeen=false,heightDone=false,resultChanged=false;
            int[] steps=(mask&2)!=0?((mask&4)!=0?new int[]{1,2,3,4,5,6}:new int[]{1,5,6}):((mask&4)!=0?new int[]{2,3,4,5,6}:new int[]{5,6});
            if(mode!=32)for(int step:steps){
                events=events*10+step;
                if(step==4)packedSeen=true;if(step==5)primary=true;if(step==6)seen=true;
                if(step==Trace.fail)break;
                if(step==3)heightDone=true;if(step==5)resultChanged=true;
            }
            long packed=((long)width<<32)|(((long)height)&0xffffffffL);
            require(Trace.events==events&&Trace.packed==(packedSeen?packed:0));
            require(Trace.width==(heightDone?~width:width)&&Trace.height==height&&Trace.offsetValue==seed);
            require(Trace.result==(resultChanged?result^0x100000001L:result));
            if(mode==32)require(failure instanceof NullPointerException);
            else require(failure==(Trace.fail==0?null:Trace.failure));
            require((Trace.partial!=null)==primary);
            long expectedOffset=(mask&2)!=0?seed:offset,expectedSize=(mask&4)!=0?result:size;
            if(primary){Core partial=(Core)Trace.partial;require(partial.image==image&&partial.offset==expectedOffset&&partial.size==expectedSize);require(value==(Trace.fail==0?partial:null));}
            else require(value==null);
            require(Trace.imageSeen==(seen?image:null)&&Trace.offsetSeen==(seen?expectedOffset:0)&&Trace.sizeSeen==(seen?expectedSize:0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events+",packed="+Long.toHexString(Trace.packed));
        }
    }
}
