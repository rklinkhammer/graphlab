ARG BASE_IMAGE
FROM ${BASE_IMAGE}
COPY lab-node /usr/local/bin/lab-node
LABEL graphlab.gate-protocol="1" graphlab.traffic-lease="1" graphlab.lab-support.language="cpp23"
ENTRYPOINT ["/usr/local/bin/lab-node"]
