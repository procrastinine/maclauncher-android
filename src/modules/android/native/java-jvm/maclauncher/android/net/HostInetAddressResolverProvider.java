package maclauncher.android.net;

import java.net.InetAddress;
import java.net.IDN;
import java.net.UnknownHostException;
import java.net.spi.InetAddressResolver;
import java.net.spi.InetAddressResolverProvider;
import java.util.Objects;
import java.util.stream.Stream;

/** Applies the Android session host policy before the JVM's ordinary resolver. */
public final class HostInetAddressResolverProvider extends InetAddressResolverProvider {
    @Override public String name(){return "MacLauncher Android resolver";}
    @Override public InetAddressResolver get(Configuration configuration){
        InetAddressResolver builtin=Objects.requireNonNull(configuration).builtinResolver();
        return new InetAddressResolver(){
            @Override public Stream<InetAddress> lookupByName(String host,LookupPolicy policy)throws UnknownHostException{
                Objects.requireNonNull(host);Objects.requireNonNull(policy);
                String policyHost;
                try{policyHost=IDN.toASCII(host);}catch(IllegalArgumentException invalid){return builtin.lookupByName(host,policy);}
                if(!HostNetworkPolicy.hostAllowed(policyHost))throw new UnknownHostException(host);
                return builtin.lookupByName(host,policy);
            }
            @Override public String lookupByAddress(byte[] address)throws UnknownHostException{
                return builtin.lookupByAddress(address);
            }
        };
    }
}
